#include <string.h>
#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "os_type.h"
#include "user_config.h"
#include "driver/uart.h"
#include "user_interface.h"
#include "espconn.h"
#include <mem.h>

#define NETWORK_UPDATE_RATE	5

#define user_procTaskQueueLen    1


#define BAUD	921600

#define TCP_PORT	8080

#define AP_CHANNEL	6
#define AP_MAX_CONNECTIONS	4
#define AP_SSID	"MicroCART"
#define AP_PSK	"m1cr0cart"

#define AP_GATEWAY	"192.168.1.1"
#define AP_NETMASK	"255.255.255.0"

#define DHCP_IP_START	"192.168.1.10"
#define DHCP_IP_END		"192.168.1.15"

#define TCP_MAX_PACKET_SIZE		1460
#define UART_TX_BUFFER_SIZE		(TCP_MAX_PACKET_SIZE * 14)
#define TCP_SEND_BUFFER_SIZE	(TCP_MAX_PACKET_SIZE * 6)

#define TCP_MAX_SEND	(500)

static volatile uint8 *_txBuffer;
static volatile uint16 _txBufferSize;
static volatile uint16 _txBufferLen;
static volatile uint8 _tcpRecvHold = 0;
static volatile int _tcpHoldCount = -10;

//TCP send
static volatile int _tcpSendActive = 0;
static volatile uint8 *_tcpSendBuffer;
static volatile uint16 _tcpSendBufferLen = 0;

//static volatile char *_messageBuffer;

static struct espconn tcpServerConn;
static struct espconn *tcpClient = NULL;

os_event_t    user_procTaskQueue[user_procTaskQueueLen];
static void uart_task(os_event_t *events);

static volatile os_timer_t networkTimer;
static volatile uint8 _ledSet;

static void startServer();

static void wifi_handler(System_Event_t *event);
static void tcp_connect_handler(void *arg);
static void tcp_disconnect_handler(void *arg);
static void tcp_recv_handler(void *arg, char *data, unsigned short len);
static void tcp_sent_handler(void *arg);
static void tcp_write_handler(void *arg);

static int tcpSend(uint8 *data, uint8 len);
static int tcpSendSome();


void network_task(void *arg) {
	static uint8_t ledState = 0;
	
	if(_ledSet) {
		ledState = 1;
		_ledSet = 0;

		//Turn on activity LED
		gpio_output_set(BIT0, 0, BIT0, 0);
	}

	if(ledState) {
		if(ledState++ > 10) {
			ledState = 0;
			gpio_output_set(0, BIT0, BIT0, 0);
		}
	}

	//Verify that there's no data "stuck" in the TCP send buffer
	if(tcpClient != NULL && !_tcpSendActive && _tcpSendBufferLen > 0) {
		//Let's send some
		if(tcpSendSome() > 0) {
			_tcpSendActive = 1;
		}
	}
}

//Task to process events
static void ICACHE_FLASH_ATTR
uart_task(os_event_t *events)
{
    switch(events->sig) {
		case UART_SIG_RECV: {
			//uint8 data[128];

			//Set the activity LED
			_ledSet = 1;

			//Echo data
			//int count = uart_get((char*)data, sizeof(data));
			int count = uart_get((char*)(_tcpSendBuffer + _tcpSendBufferLen),
				TCP_SEND_BUFFER_SIZE - _tcpSendBufferLen);
			
			_tcpSendBufferLen += count;

			//Send over TCP
			if(tcpClient != NULL && !_tcpSendActive) {
				_tcpSendActive = 1;

				tcpSendSome();
			}
		}
    break;
		
		case UART_SIG_TXTO:
			//Transmit FIFO near empty

			if(_txBufferLen > 0) {
				//Fill the FIFO
				uint16 sent = uart0_send_nowait((uint8*)_txBuffer, _txBufferLen);

				if(sent < _txBufferLen) {
					memmove((uint8*)_txBuffer, (uint8*)_txBuffer + sent,
						_txBufferLen - sent);
					_txBufferLen -= sent;

					uart_set_txto();
				}
				else {
					_txBufferLen = 0;
					uart_clear_txto();
				}

				//Check if receive hold is active
				if((_tcpRecvHold == 1) && (tcpClient != NULL) &&
					((_txBufferSize - _txBufferLen) >= (10*TCP_MAX_PACKET_SIZE)) ) {
					espconn_recv_unhold(tcpClient);
					_tcpRecvHold = 0;

					_tcpHoldCount--;
				}
			}
		break;

    default:
      os_delay_us(10);
      break;
    }

		//os_delay_us(100);
}

void wifi_init() {
	//Set to SoftAP mode
	wifi_set_opmode(SOFTAP_MODE);

	//SoftAP configuration
	struct softap_config apConfig = {
		.channel = AP_CHANNEL,
		.authmode = AUTH_WPA2_PSK,
		.ssid_hidden = 0,
		.max_connection = AP_MAX_CONNECTIONS,
		.beacon_interval = 100
	};
	strcpy(apConfig.ssid, AP_SSID);
	strcpy(apConfig.password, AP_PSK);
	apConfig.ssid_len = strlen(AP_SSID);

	//Set configuration
	wifi_softap_set_config(&apConfig);

	//Disable power saving mode
	//This seems to help reduce jitter in latency
	//wifi_set_sleep_type(NONE_SLEEP_T);

	//DHCP configuration
	struct dhcps_lease dhcpLease = {
		.start_ip = ipaddr_addr(DHCP_IP_START),
		.end_ip = ipaddr_addr(DHCP_IP_END)
	};

	//Disable DHCP server while making changes
	wifi_softap_dhcps_stop();
	wifi_softap_set_dhcps_lease(&dhcpLease);

	//Set IP info
	struct ip_info ipInfo = {
		.ip = ipaddr_addr(AP_GATEWAY),
		.gw = ipaddr_addr(AP_GATEWAY),
		.netmask = ipaddr_addr(AP_NETMASK)
	};
	wifi_set_ip_info(SOFTAP_IF, &ipInfo);
	
	//Restart DHCP server
	wifi_softap_dhcps_start();

	//Set WiFi event handler
	wifi_set_event_handler_cb(&wifi_handler);

	startServer();
}

void startServer() {
	//Configure TCP server
	tcpServerConn.type = ESPCONN_TCP;
	tcpServerConn.state = ESPCONN_NONE;
	tcpServerConn.proto.tcp = (esp_tcp*)os_malloc(sizeof(esp_tcp));
	tcpServerConn.proto.tcp->local_port = TCP_PORT;

	espconn_regist_connectcb(&tcpServerConn, &tcp_connect_handler);

	tcpClient = NULL;

	//Start TCP server
	espconn_accept(&tcpServerConn);

	//Set maximum connection count
	espconn_tcp_set_max_con(1);
}

void wifi_handler(System_Event_t *event) {
	switch(event->event) {
		case EVENT_SOFTAPMODE_STACONNECTED:
			//TODO: Perhaps do something with this information
		break;

		case EVENT_SOFTAPMODE_STADISCONNECTED:
			//TODO: Maybe do something here?
		break;

		default:
			break;
	}
}

void tcp_connect_handler(void *arg) {
	struct espconn *conn = (struct espconn*)arg;
	
	espconn_regist_disconcb(conn, &tcp_disconnect_handler);
	espconn_regist_recvcb(conn, &tcp_recv_handler);
	espconn_regist_sentcb(conn, &tcp_sent_handler);
	espconn_regist_write_finish(conn, &tcp_write_handler);
	
	//Set socket options
	espconn_set_opt(conn, ESPCONN_NODELAY | ESPCONN_REUSEADDR | ESPCONN_KEEPALIVE | ESPCONN_COPY);

	tcpClient = conn;
	_tcpRecvHold = 0;

	_tcpSendActive = 0;
	_tcpSendBufferLen = 0;
}

void tcp_disconnect_handler(void *arg) {
	tcpClient = NULL;

	char msg[128];
	os_sprintf("tcp_disconnect_handler: %d\r\n", (int)_tcpSendBufferLen);
	uart0_send_nowait(msg, strlen(msg));

	_tcpSendBufferLen = 0;
	_tcpSendActive = 0;
}

void tcp_recv_handler(void *arg, char *data, unsigned short len) {
	//Send over UART
	
	//Copy into UART tx buffer
	if( (_txBufferLen + len) > _txBufferSize ) {
		//Yikes!
		espconn_recv_hold((struct espconn*)arg);
		_tcpRecvHold = 1;
	}
	else {
		if(_txBufferLen == 0) {
			//Software buffer is empty

			//Put as much as possible into FIFO
			uint16 sent = uart0_send_nowait(data, len);

			if(sent < len) {
				//Put the rest in the software buffer
				memcpy((uint8*)_txBuffer, data + sent, len - sent);
				_txBufferLen = len - sent;
				uart_set_txto();
			}
		}
		else {
			//Just copy all of the data into the software buffer
			memcpy((uint8*)_txBuffer + _txBufferLen, data, len);
			_txBufferLen += len;
			uart_set_txto();
		}

		if( (_tcpRecvHold == 0) &&
			(_txBufferSize - _txBufferLen) < (10*TCP_MAX_PACKET_SIZE) ) {
			//If we can't fit another packet into the software buffer
			//Temporarily disable receiving
			espconn_recv_hold((struct espconn*)arg);
			_tcpRecvHold = 1;

			_tcpHoldCount++;
			
		}
	}


	_ledSet = 1;
}

int tcpSendSome() {
	if(_tcpSendBufferLen == 0) {
		return 0;
	}
	else if(tcpClient == NULL) {
		return 0;
	}
	
	uint16 sendAmt = (_tcpSendBufferLen < TCP_MAX_SEND) ? (_tcpSendBufferLen) : (TCP_MAX_SEND);

	sint8 retval = espconn_sent(tcpClient, (uint8*)_tcpSendBuffer, sendAmt);

	if(retval != 0) {
		/*
		char buffer[128];
		char *msg;

		if(retval == ESPCONN_MEM) {
			msg = "[Error] espconn_sent returned ESPCONN_MEM\r\n";
		}
		else if(retval == ESPCONN_ARG) {
			msg = "[Error] espconn_sent returned ESPCONN_ARG\r\n";
		}
		else {
			os_sprintf(buffer, "[Error] espconn_sent returned an unknown error: %d\r\n", (int)retval);
			msg = buffer;
		}
		uart0_send_nowait(msg, strlen(msg));
		
		
		char msg[128];
		os_sprintf("tcpSendSome: Failed to send (%d)\r\n", (int)_tcpSendBufferLen);
		uart0_send_nowait(msg, strlen(msg));
		*/
		//Error, nothing was sent
		return 0;
	}
	else {
		//Remove the sent data from the buffer
		memmove((uint8*)_tcpSendBuffer, (uint8*)_tcpSendBuffer + sendAmt, (uint16)_tcpSendBufferLen - sendAmt);
		_tcpSendBufferLen -= sendAmt;
/*
		char msg[128];
		os_sprintf("tcpSendSome: Sent %d (%d left)\r\n", (int)sendAmt, (int)_tcpSendBufferLen);
		uart0_send_nowait(msg, strlen(msg));
*/
		return sendAmt;
	}
}

void tcp_sent_handler(void *arg) {
	//Nothing to do here
	char msg[128];
	os_sprintf(msg, "tcp_sent_handler: %d, %d\r\n", (int)_tcpSendActive, (int)_tcpSendBufferLen);
	uart0_send_nowait(msg, strlen(msg));
}

void tcp_write_handler(void *arg) {
	if(tcpClient == NULL) {
		return;
	}

	//Debug stuff
	char msg[128];
	os_sprintf(msg, "tcp_write_handler: %d, %d\r\n", (int)_tcpSendActive, (int)_tcpSendBufferLen);
	uart0_send_nowait(msg, strlen(msg));

	if(_tcpSendBufferLen > 0) {
		//Send some more
		int sent = tcpSendSome();

		if(sent == 0 || _tcpSendBufferLen == 0) {
			_tcpSendActive = 0;
		}
	}
	else {
		_tcpSendActive = 0;
	}
}

//Init function 
void ICACHE_FLASH_ATTR
user_init()
{
    //Remove debug statements from UART0
    system_set_os_print(0);
		
		// Initialize the GPIO subsystem.
    gpio_init();

    //Set GPIO2 to output mode
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2);

    //Set GPIO2 low
    gpio_output_set(0, BIT0, BIT0, 0);

		//Initialize UART
		uart_init(BAUD, BAUD);

		//Initialize UART TX buffer
		_txBuffer = (uint8*)os_malloc(UART_TX_BUFFER_SIZE);
		_txBufferSize = UART_TX_BUFFER_SIZE;
		_txBufferLen = 0;

		//Initialize TCP send buffer
		_tcpSendBuffer = (uint8*)os_malloc(TCP_SEND_BUFFER_SIZE);
		_tcpSendBufferLen = 0;
		_tcpSendActive = 0;

		//Initialize WiFi
		wifi_init();

		//Start TCP server
		startServer();

		_ledSet = 0;

    //Disarm timer
    os_timer_disarm(&networkTimer);

    //Setup timer
    os_timer_setfn(&networkTimer, (os_timer_func_t *)network_task, NULL);

    //Arm the timer
    os_timer_arm(&networkTimer, NETWORK_UPDATE_RATE, 1);
    
    //Start os task
    system_os_task(uart_task, UART_TASK_PRIORITY,user_procTaskQueue,
			user_procTaskQueueLen);
}
