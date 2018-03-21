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
#include "driver/gpio16.h"
#include <mem.h>

#define NETWORK_UPDATE_RATE	5
#define SWITCH_DEBOUNCE_TIME	500

#define user_procTaskQueueLen    10


#define BAUD	115200

#define TCP_PORT	288

#define AP_MAX_CONNECTIONS	8
#define AP_PSK	"cpre288psk"

#define AP_GATEWAY	"192.168.4.1"
#define AP_NETMASK	"255.255.255.0"

#define DHCP_IP_START	"192.168.4.10"
#define DHCP_IP_END		"192.168.4.15"

#define UART_RX_BUFFER_SIZE		(1024)
#define UART_TX_BUFFER_SIZE		(128)

static uint8 *_rxBuffer;
static uint8 *_txBuffer;

os_event_t    user_procTaskQueue[user_procTaskQueueLen];
static void uart_task(os_event_t *events);
static void tcp_recvHandler(uint16 len);

static void wifi_start();
static void wifi_stop();

static volatile os_timer_t networkTimer, switchDebounceTimer;
static int switchState, switchValid;
static volatile uint8 _ledSet;

static void wifi_handler(System_Event_t *event);

static volatile int uartCount = 0;
static volatile uint8 _uartTxFlag;

static const int ADDR_PIN_NAMES[] = {
	PERIPHS_IO_MUX_MTMS_U,		//GPIO14
	PERIPHS_IO_MUX_MTDI_U,		//GPIO12
	PERIPHS_IO_MUX_GPIO4_U,		//GPIO04
	PERIPHS_IO_MUX_MTCK_U		//GPIO13
};
static const int LED_PIN_NAME = PERIPHS_IO_MUX_GPIO2_U;
static const int SWITCH_PIN_NAME = PERIPHS_IO_MUX_GPIO5_U;

static const int ADDR_PIN_FUNCS[] = {
	FUNC_GPIO14,
	FUNC_GPIO12,
	FUNC_GPIO4,
	FUNC_GPIO13
};
static const int LED_PIN_FUNC = FUNC_GPIO2;
static const int SWITCH_PIN_FUNC = FUNC_GPIO5;

static const int ADDR_PINS[] = { 14, 12, 4, 13 };
static const int LED_PIN = 2;
static const int SWITCH_PIN = 5;

static uint8_t WIFI_CHANNELS[] = {1, 6, 11};
static int WIFI_CHANNEL_COUNT = 3;

static void user_gpio_init() {
	//ADDR_0 - ADDR_3
	int i;
	for(i = 0; i < 4; ++i) {
		PIN_FUNC_SELECT(ADDR_PIN_NAMES[i], ADDR_PIN_FUNCS[i]);
		PIN_PULLUP_EN(ADDR_PIN_NAMES[i]);
	}

	//LED
	PIN_FUNC_SELECT(LED_PIN_NAME, LED_PIN_FUNC);
	GPIO_OUTPUT_SET(LED_PIN, 1);

	//Switch
	PIN_FUNC_SELECT(SWITCH_PIN_NAME, SWITCH_PIN_FUNC);
}

static uint8_t getDeviceID() {
	uint8_t address = 0;

	int i;
	for(i = 0; i < 4; ++i) {
		if(!GPIO_INPUT_GET(ADDR_PINS[i])) {
			address |= 1 << i;
		}
	}

	return address;
}

static const char* getSSID() {
	static char id[9] = {'c', 'y', 'B', 'O', 'T', ' ', 0, 0, 0};
	
	uint8_t idNum = getDeviceID();

	if(idNum > 9) {
		id[6] = (idNum/10) + '0';
		id[7] = (idNum % 10) + '0';
	}
	else {
		id[6] = idNum + '0';
	}

	return id;
}

void network_task(void *arg) {
	static uint8_t ledState = 0;

	int switchValue = !GPIO_INPUT_GET(SWITCH_PIN);
	if((switchValue != switchState) && switchValid) {
		switchState = switchValue;
		if(switchState) {
			wifi_start();
		}
		else {
			wifi_stop();
		}

		switchValid = 0;
		_ledSet = 1;

		os_timer_arm(&switchDebounceTimer, SWITCH_DEBOUNCE_TIME, 0);
	}

	if(_ledSet) {
    if(switchState) {
  		ledState = 1;
			
      //Turn on activity LED
		  GPIO_REG_WRITE(GPIO_OUT_W1TC_ADDRESS, 1 << LED_PIN);
    }
    
    _ledSet = 0;
	}

	if(ledState) {
		if(ledState++ > 10) {
			ledState = 0;
			GPIO_REG_WRITE(GPIO_OUT_W1TS_ADDRESS, 1 << LED_PIN);
		}
	}
}

void switch_debounce_task() {
	switchValid = 1;
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

void wifi_start() {
	//Set to SoftAP mode
	wifi_set_opmode(SOFTAP_MODE);

	//SoftAP configuration
	struct softap_config apConfig = {
		.channel = WIFI_CHANNELS[getDeviceID() % WIFI_CHANNEL_COUNT],
		.authmode = AUTH_WPA2_PSK,
		.ssid_hidden = 0,
		.max_connection = AP_MAX_CONNECTIONS,
		.beacon_interval = 100
	};

	const char* ssid = getSSID();
	strcpy(apConfig.ssid, ssid);
	strcpy(apConfig.password, AP_PSK);
	apConfig.ssid_len = strlen(ssid);

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

void wifi_stop() {
	wifi_set_opmode(NULL_MODE);
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

		//Initialize UART
		uart_init(BAUD, BAUD);

		//Initialize user GPIO pins
		user_gpio_init();

		_rxBuffer = (uint8*)os_malloc(UART_RX_BUFFER_SIZE);
		_txBuffer = (uint8*)os_malloc(UART_TX_BUFFER_SIZE);

		_uartTxFlag = 0;

		//Turn of AP
		wifi_stop();

		//Start TCP server
		tcp_start(TCP_PORT);
		tcp_setRecvHandler(&tcp_recvHandler);

		_ledSet = 0;
		switchState = 0;
		switchValid = 1;

    //Disarm timer
    os_timer_disarm(&networkTimer);

    //Setup timer
    os_timer_setfn(&networkTimer, (os_timer_func_t *)network_task, NULL);
    os_timer_setfn(&switchDebounceTimer, (os_timer_func_t *)switch_debounce_task, NULL);

    //Arm the timer
    os_timer_arm(&networkTimer, NETWORK_UPDATE_RATE, 1);

    //Start os task
    system_os_task(uart_task, UART_TASK_PRIORITY,user_procTaskQueue,
			user_procTaskQueueLen);
}
