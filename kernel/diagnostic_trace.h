#ifndef VITA_NS_DIAGNOSTIC_TRACE_H
#define VITA_NS_DIAGNOSTIC_TRACE_H

#include <stdint.h>

#define DIAGNOSTIC_TRACE_CAPACITY 256U
#define DIAGNOSTIC_TRACE_DATA_CAPACITY 32U

typedef struct {
	volatile unsigned int sequence;
	uint16_t length;
	uint8_t type;
	uint8_t data[DIAGNOSTIC_TRACE_DATA_CAPACITY];
} DiagnosticTraceEvent;

void diagnostic_trace_reset(void);
void diagnostic_trace_publish(uint8_t type, const uint8_t *data, int length);
const DiagnosticTraceEvent *diagnostic_trace_event(unsigned int index);

#endif
