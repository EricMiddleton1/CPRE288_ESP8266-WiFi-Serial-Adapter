#include <string.h>
#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "os_type.h"
#include "user_config.h"
#include "driver/uart.h"
#include "user_interface.h"
#include "espconn.h"
#include "user_tcp.h"
#include <mem.h>

#define NETWORK_UPDATE_RATE	5

#define user_procTaskQueueLen    10


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

#define UART_RX_BUFFER_SIZE		(1024)
#define UART_TX_BUFFER_SIZE		(128)

static uint8 *_rxBuffer;
static uint8 *_txBuffer;

os_event_t    user_procTaskQueue[user_procTaskQueueLen];
static void uart_task(os_event_t *events);
static void tcp_recvHandler(uint16 len);

static volatile os_timer_t networkTimer;
static volatile uint8 _ledSet;

static void wifi_handler(System_Event_t *event);

static volatile int uartCount = 0;
static volatile uint8 _uartTxFlag;

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

/*
	static int i = 0;
	if(i == 0) {
		i = 100;

		char msg[128];
		os_sprintf(msg, "[Heap] %d\r\n", 
			(int)system_get_free_heap_size());
		uart_debugSend(msg);
	}
	i--;
*/
}

//Task to process events
//static void ICACHE_FLASH_ATTR
static void uart_task(os_event_t *events)
{
    switch(events->sig) {
		case UART_SIG_ERR_FRM: {
			uart_debugSend("UART RX Frame error\r\n");
		}
		break;

		case UART_SIG_ERR_PARITY: {
			uart_debugSend("UART RX Parity error\r\n");
		}
		break;

		case UART_SIG_RXOVF: {
			uart_debugSend("UART RX overflow\r\n");
		}
		case UART_SIG_RECV: {
			static int bytesRecv = 0;

			//Set the activity LED
			_ledSet = 1;

			//Echo data
			int count = uart_get((char*)_rxBuffer, UART_RX_BUFFER_SIZE);

			bytesRecv += count;
			
			if(count > 0) {
				tcp_send((uint8*)_rxBuffer, count);

				uartCount = 0;

			}
		}
    break;


		
		case UART_SIG_TXTO: {
			//Transmit FIFO near empty

			//Grab as much as we can send
			uint8 sendSpace = uart_getTxFifoAvail();
			uint8 toSend = tcp_receive(_txBuffer, sendSpace);

			if(toSend > 0) {
				//Fill the FIFO
				uart0_send_nowait(_txBuffer, toSend);

				uart_set_txto();
				_ledSet = 1;
			}
			else {
				uart_clear_txto();

				_uartTxFlag = 0;
			}
		}
		break;

    default:
      //os_delay_us(10);
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

void tcp_recvHandler(uint16 len) {
	if(_uartTxFlag == 0) {
		//Grab as much as we can currently send
		uint8 sendSpace = uart_getTxFifoAvail();
		uint8 toSend = tcp_receive(_txBuffer, sendSpace);

		uart0_send_nowait(_txBuffer, toSend);
		uart_set_txto();

		_uartTxFlag = 1;
		_ledSet = 1;
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

		_rxBuffer = (uint8*)os_malloc(UART_RX_BUFFER_SIZE);
		_txBuffer = (uint8*)os_malloc(UART_TX_BUFFER_SIZE);

		_uartTxFlag = 0;

		//Initialize WiFi
		wifi_init();

		//Start TCP server
		tcp_start(TCP_PORT);
		tcp_setRecvHandler(&tcp_recvHandler);

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
