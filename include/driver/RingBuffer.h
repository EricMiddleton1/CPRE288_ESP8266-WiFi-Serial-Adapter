#pragma once

#include "os_type.h"

typedef struct {
	uint8 *buffer;
	uint16 front, back, size;
} RingBuffer;


void RingBuffer_init(RingBuffer *buffer, uint16 size);

void RingBuffer_addByte(RingBuffer *buffer, uint8 byte);

void RingBuffer_put(RingBuffer *buffer, uint8 *data, uint16 len);

void RingBuffer_clear(RingBuffer *buffer);

uint16_t RingBuffer_getSize(RingBuffer *buffer);

uint16_t RingBuffer_get(RingBuffer *buffer, uint8 *out, uint16 outSize);
