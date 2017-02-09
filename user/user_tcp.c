#include "user_tcp.h"

#include "ip_addr.h"
#include "osapi.h"
#include "espconn.h"
#include <mem.h>
#include <string.h>

//Debugging
#include "driver/uart.h"

#define TCP_MAX_PACKET	(1460)
#define TCP_SEND_BUFFER_SIZE	(4096)
#define TCP_RECV_BUFFER_SIZE	(4*TCP_MAX_PACKET)

#define TCP_RECV_HOLD_LIMIT	(5*TCP_MAX_PACKET)

#define MAX_SEND_COUNT	(7)	

struct Connection {
	struct espconn *pConn;

	uint8 *sendBuffer;
	uint16 sendSize, sendLen;

	ReceiveHandler recvHandler;

	int sendCount;
};

static struct espconn _tcpServer;

static struct Connection _tcpConn;

//Callbacks
static void __connectHandler(void *arg);
static void __disconnectHandler(void *arg);
static void __reconnectHandler(void *arg, sint8 err);
static void __recvHandler(void *arg, char *data, unsigned short len);
static void __sentHandler(void *arg);
static void __writeHandler(void *arg);

static uint16 __send(struct Connection *conn, uint8 *data, uint16 len);


void tcp_start(uint16 port) {
	_tcpConn.sendBuffer = (uint8*)os_malloc(TCP_SEND_BUFFER_SIZE);
	_tcpConn.sendSize = TCP_SEND_BUFFER_SIZE;
	_tcpConn.sendLen = 0;

	_tcpConn.pConn = NULL;

	_tcpServer.type = ESPCONN_TCP;
	_tcpServer.state = ESPCONN_NONE;
	_tcpServer.proto.tcp = (esp_tcp*)os_malloc(sizeof(esp_tcp));
	_tcpServer.proto.tcp->local_port = port;
	
	espconn_regist_connectcb(&_tcpServer, &__connectHandler);

	espconn_accept(&_tcpServer);
	espconn_tcp_set_max_con(1);
}

void tcp_stop() {
	//TODO
}

void tcp_setRecvHandler(ReceiveHandler handler) {
	_tcpConn.recvHandler = handler;
}

void tcp_send(uint8* buffer, uint16 len) {
	if(len == 0) {
		uart_debugSend("[tcp_send] Given buffer length 0\r\n");
		
		return;
	}

	if(_tcpConn.pConn != NULL) {
		if((_tcpConn.sendCount < MAX_SEND_COUNT) && (_tcpConn.sendLen == 0)) {
			uint16 sendAmt = __send(&_tcpConn, buffer, len);

			if(sendAmt < len) {
				memmove(_tcpConn.sendBuffer + _tcpConn.sendLen, buffer + sendAmt, len - sendAmt);
				_tcpConn.sendLen = len - sendAmt;
			}
		}
		else {
			memmove(_tcpConn.sendBuffer + _tcpConn.sendLen, buffer, len);
			_tcpConn.sendLen += len;

			char msg[128];
			//os_sprintf(msg, "[tcp_send] Defferred (%d)\r\n", (int)_tcpConn.sendLen);
			//__debug(msg);
		}
	}
	else {
		uart_debugSend("[Send] (Not connected)\r\n");
	}
}

uint16 __send(struct Connection *conn, uint8 *data, uint16 len) {
	conn->sendCount++;

	uint16 sendAmt = len;
	if(sendAmt > TCP_MAX_PACKET)
		sendAmt = TCP_MAX_PACKET;
	
	if(conn->pConn == NULL) {
		uart_debugSend("[__send] NULL espconn\r\n");

		return 0;
	}
	
	sint8 retval = espconn_send(conn->pConn, data, sendAmt);
	
	if(retval == ESPCONN_ARG) {
		char msg[128];
		os_sprintf(msg, "[__send] ESPCONN_ARG: %d, %d\r\n", (int)data, (int)sendAmt);
		uart_debugSend(msg);

		return 0;
	}
	else if(retval != 0) {
		char msg[128];
		os_sprintf(msg, "[__send] (%d, %d)\r\n", (int)retval, (int)(conn->sendCount));
		uart_debugSend(msg);

		sendAmt = 0;
	}
	else {
		char msg[128];
		os_sprintf(msg, "[__send] %d sent\r\n", (int)sendAmt);
		uart_debugSend(msg);
	}

	return sendAmt;
}

void __connectHandler(void *arg) {
	struct espconn *conn = (struct espconn*)arg;

	_tcpConn.pConn = conn;
	conn->reverse = &_tcpConn;
	_tcpConn.sendLen = 0;

	//Register handlers
	espconn_regist_disconcb(conn, &__disconnectHandler);
	espconn_regist_reconcb(conn, &__reconnectHandler);
	espconn_regist_recvcb(conn, &__recvHandler);
	espconn_regist_sentcb(conn, &__sentHandler);
	espconn_regist_write_finish(conn, &__writeHandler);

	//Set socket options
	espconn_set_opt(conn, ESPCONN_NODELAY | ESPCONN_KEEPALIVE | ESPCONN_COPY);

	//Clear tcpConn structure
	RingBuffer_clear(&(_tcpConn.sendBuffer));
	_tcpConn.sendCount = 0;
}

void __disconnectHandler(void *arg) {
	struct Connection *conn = (struct Connection*)(((struct espconn*)arg)->reverse);

	conn->pConn = NULL;

	uart_debugSend("[Disconnect]\r\n");
}

void __reconnectHandler(void *arg, sint8 err) {
	char msg[128];

	os_sprintf(msg, "[Reconnect] (%d)\r\n", (int)err);
	uart_debugSend(msg);
}

void __recvHandler(void *arg, char *data, unsigned short len) {
	struct Connection *conn = (struct Connection*)(((struct espconn*)arg)->reverse);

	if(conn->recvHandler != NULL) {
		conn->recvHandler(data, len);
	}

	//TODO: Hold on/off
}

void __sentHandler(void *arg) {
	struct Connection *conn = (struct Connection*)(((struct espconn*)arg)->reverse);
	
	conn->sendCount--;

	if((conn->sendCount < MAX_SEND_COUNT) && (conn->sendLen > 0)) {
		uint16 sendAmt = __send(conn, conn->sendBuffer, conn->sendLen);
		conn->sendLen -= sendAmt;

		//char msg[128];
		//os_sprintf(msg, "[sentHandler] Sent %d (%d)\r\n", (int)sendAmt, (int)conn->sendLen);
		//__debug(msg);
	}
}

void __writeHandler(void *arg) {
	struct Connection *conn = (struct Connection*)(((struct espconn*)arg)->reverse);
}