#include "diagnostic_trace.h"

#include <string.h>

static volatile unsigned int write_index;
static DiagnosticTraceEvent events[DIAGNOSTIC_TRACE_CAPACITY];

void diagnostic_trace_reset(void)
{
	write_index = 0;
	memset(events, 0, sizeof(events));
}

void diagnostic_trace_publish(uint8_t type, const uint8_t *data, int length)
{
	unsigned int index = __sync_fetch_and_add(&write_index, 1);
	DiagnosticTraceEvent *entry =
		&events[index % DIAGNOSTIC_TRACE_CAPACITY];
	unsigned int copy_length = length <= 0 ? 0U :
		(length > (int)sizeof(entry->data) ?
		 (unsigned int)sizeof(entry->data) : (unsigned int)length);
	entry->type = type;
	entry->length = length;
	if (copy_length && data)
		memcpy(entry->data, data, copy_length);
	__sync_synchronize();
	entry->sequence = index + 1;
}

const DiagnosticTraceEvent *diagnostic_trace_event(unsigned int index)
{
	return &events[index % DIAGNOSTIC_TRACE_CAPACITY];
}
