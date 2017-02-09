#include "driver/RingBuffer.h"

#include <mem.h>
#include <string.h>	//memcpy

void RingBuffer_init(RingBuffer *buffer, uint16 size) {
	buffer->front = 0;
	buffer->back = 0;
	buffer->size = size;

	buffer->buffer = (uint8*)os_malloc(size);
}

void RingBuffer_addByte(RingBuffer *buffer, uint8 byte) {
	buffer->buffer[buffer->back] = byte;

	buffer->back = (buffer->back + 1) & (buffer->size - 1);
}

void RingBuffer_clear(RingBuffer *buffer) {
	buffer->front = buffer->back = 0;
}

uint16_t RingBuffer_getSize(RingBuffer *buffer) {
	return (buffer->back >= buffer->front) ? (buffer->back - buffer->front)
		: (buffer->size + buffer->back - buffer->front);
}

uint16_t RingBuffer_get(RingBuffer *buffer, uint8 *out, uint16 outSize) {
	uint16_t cpyAmt = RingBuffer_getSize(buffer);
	if(cpyAmt > outSize)
		cpyAmt = outSize;

	if( (buffer->front + cpyAmt) > buffer->size ) {
		uint16_t cpyRemainder = buffer->front + cpyAmt - buffer->size;
		//Two copies
		memcpy(out, buffer->buffer + buffer->front, cpyAmt - cpyRemainder);
		memcpy(out + cpyAmt - cpyRemainder, buffer->buffer, cpyRemainder);
	}
	else {
		//One copy
		memcpy(out, buffer->buffer + buffer->front, cpyAmt);
	}

	buffer->front = (buffer->front + cpyAmt) & (buffer->size - 1);

	return cpyAmt;
}
