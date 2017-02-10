#pragma once

#include "os_type.h"

typedef void (*ReceiveHandler)(uint16 len);

void tcp_start(uint16 port);
void tcp_stop();

void tcp_setRecvHandler(ReceiveHandler handler);

void tcp_send(uint8* buffer, uint16 len);
uint16 tcp_receive(uint8* buffer, uint16 size);
