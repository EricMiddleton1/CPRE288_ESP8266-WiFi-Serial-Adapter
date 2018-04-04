#include "user_tcp.h"

#include "ip_addr.h"
#include "osapi.h"
#include "espconn.h"
#include "os_type.h"
#include <mem.h>
#include <string.h>

//Debugging
#include "driver/uart.h"

#define TCP_MAX_PACKET	(1460)
#define TCP_SEND_BUFFER_SIZE	(1024 * 20)
#define TCP_RECV_BUFFER_SIZE	(10*TCP_MAX_PACKET)

#define TCP_RECV_HOLD_LIMIT	(5*TCP_MAX_PACKET)
#define TCP_TIMEOUT		(7200)

#define MAX_SEND_COUNT	(2)

struct Connection {
	struct espconn *pConn;

	uint8 *sendBuffer;
	uint16 sendSize, sendLen;

	uint8 *recvBuffer;
	uint16 recvSize, recvLen;
	uint8 recvHold;

	ReceiveHandler recvHandler;

	int sendCount;
};

static struct espconn _tcpServer;

static struct Connection _tcpConn;

static os_timer_t _sendTimer;

//Callbacks
static void __connectHandler(void *arg);
static void __disconnectHandler(void *arg);
static void __reconnectHandler(void *arg, sint8 err);
static void __recvHandler(void *arg, char *data, unsigned short len);
static void __sentHandler(void *arg);
static void __writeHandler(void *arg);

static void __sendTimerHandler(void *arg);

static uint16 __send(struct Connection *conn, uint8 *data, uint16 len);


void tcp_start(uint16 port) {
	os_timer_disarm(&_sendTimer);
	os_timer_setfn(&_sendTimer, (os_timer_func_t*)__sendTimerHandler, NULL);

	_tcpConn.sendBuffer = (uint8*)os_malloc(TCP_SEND_BUFFER_SIZE);
	_tcpConn.sendSize = TCP_SEND_BUFFER_SIZE;
	_tcpConn.sendLen = 0;

	_tcpConn.recvBuffer = (uint8*)os_malloc(TCP_RECV_BUFFER_SIZE);
	_tcpConn.recvSize = TCP_RECV_BUFFER_SIZE;
	_tcpConn.recvLen = 0;
	_tcpConn.recvHold = 0;

	_tcpConn.pConn = NULL;

	_tcpServer.type = ESPCONN_TCP;
	_tcpServer.state = ESPCONN_NONE;
	_tcpServer.proto.tcp = (esp_tcp*)os_malloc(sizeof(esp_tcp));
	_tcpServer.proto.tcp->local_port = port;
	
	espconn_regist_connectcb(&_tcpServer, &__connectHandler);
	espconn_regist_time(&_tcpServer, TCP_TIMEOUT, ESPCONN_KEEPINTVL);

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
				if((sendAmt + _tcpConn.sendLen) > _tcpConn.sendSize) {
					uart_debugSend("[tcp_send] send buffer full!\r\n");
				}
				else {
					memcpy(_tcpConn.sendBuffer, buffer + sendAmt, len - sendAmt);
					_tcpConn.sendLen = len - sendAmt;
				}
			}
		}
		else {
			if((_tcpConn.sendLen + len) > _tcpConn.sendSize) {
				uart_debugSend("[tcp_send] send buffer full!\r\n");
			}
			else {
				memcpy(_tcpConn.sendBuffer + _tcpConn.sendLen, buffer, len);
				_tcpConn.sendLen += len;
			}

			//char msg[128];
			//os_sprintf(msg, "[tcp_send] Defferred (%d)\r\n", (int)_tcpConn.sendLen);
			//__debug(msg);
		}
	}
	else {
		uart_debugSend("[Send] (Not connected)\r\n");
	}
}

uint16 tcp_receive(uint8 *buffer, uint16 size) {
	uint16 recvAmt = _tcpConn.recvLen;
	if(recvAmt > size)
		recvAmt = size;
	
	memcpy(buffer, _tcpConn.recvBuffer, recvAmt);

	if(recvAmt != _tcpConn.recvLen) {
		memmove(_tcpConn.recvBuffer, _tcpConn.recvBuffer + recvAmt, (_tcpConn.recvLen - recvAmt));
	}

	_tcpConn.recvLen -= recvAmt;

	if( (_tcpConn.recvHold == 1) && (_tcpConn.recvLen < TCP_RECV_HOLD_LIMIT) ) {
		espconn_recv_unhold(_tcpConn.pConn);
		_tcpConn.recvHold = 0;
	}

	return recvAmt;
}

uint16 __send(struct Connection *conn, uint8 *data, uint16 len) {
	//conn->sendCount++;

	uint16 sendAmt = len;
	if(sendAmt > TCP_MAX_PACKET)
		sendAmt = TCP_MAX_PACKET;


	if(conn->pConn == NULL) {
		uart_debugSend("[__send] NULL espconn\r\n");

		return 0;
	}
	
	sint8 retval = espconn_send(conn->pConn, data, sendAmt);
/*
	char msg[128];
	os_sprintf(msg, "[__send] Sent %d\r\n", (int)sendAmt);
	uart_debugSend(msg);
*/	

	if((retval != 0) && (conn->sendCount == 0)) {
		os_timer_arm(&_sendTimer, 1, 0);
	}

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
		//char msg[128];
		//os_sprintf(msg, "[__send] %d sent\r\n", (int)sendAmt);
		//uart_debugSend(msg);

		conn->sendCount++;
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
	espconn_regist_time(conn, TCP_TIMEOUT, ESPCONN_KEEPINTVL);


	//Set socket options
	espconn_set_opt(conn, ESPCONN_NODELAY | ESPCONN_COPY);

	//Clear tcpConn structure
	//RingBuffer_clear(&(_tcpConn.sendBuffer));
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
	
	memcpy(conn->recvBuffer + conn->recvLen, data, len);
	conn->recvLen += len;

	if( (conn->recvLen > TCP_RECV_HOLD_LIMIT) && (conn->recvHold == 0) ) {
		espconn_recv_hold(conn->pConn);
		conn->recvHold = 1;
	}

	if(conn->recvHandler != NULL) {
		conn->recvHandler(len);
	}
}

void __sentHandler(void *arg) {
	struct Connection *conn = (struct Connection*)(((struct espconn*)arg)->reverse);
	
	conn->sendCount--;

/*
	char msg[128];
	os_sprintf(msg, "[__sentHandler] %d\r\n", (int)conn->sendCount);
	uart_debugSend(msg);
*/

	if((conn->sendCount < MAX_SEND_COUNT) && (conn->sendLen > 0)) {
		uint16 sendAmt = __send(conn, conn->sendBuffer, conn->sendLen);
		
		if(sendAmt != 0) {
			if(sendAmt != conn->sendLen) {
				memmove(conn->sendBuffer, conn->sendBuffer + sendAmt,
					conn->sendLen - sendAmt);
			}
			conn->sendLen -= sendAmt;
		}

		//char msg[128];
		//os_sprintf(msg, "[sentHandler] Sent %d (%d)\r\n", (int)sendAmt, (int)conn->sendLen);
		//__debug(msg);
	}
}

void __writeHandler(void *arg) {
	struct Connection *conn = (struct Connection*)(((struct espconn*)arg)->reverse);
}

void __sendTimerHandler(void *arg) {
	if((_tcpConn.sendCount < MAX_SEND_COUNT) && (_tcpConn.sendLen > 0)) {
		uint16 sendAmt = __send(&_tcpConn, _tcpConn.sendBuffer, _tcpConn.sendLen);

		if(sendAmt != 0) {
			if(sendAmt != _tcpConn.sendLen) {
				memmove(_tcpConn.sendBuffer, _tcpConn.sendBuffer + sendAmt,
					_tcpConn.sendLen - sendAmt);
			}
			_tcpConn.sendLen -= sendAmt;
		}
	}
}
