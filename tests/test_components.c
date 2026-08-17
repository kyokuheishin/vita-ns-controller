#include "bond_record.h"
#include "bond_state.h"
#include "diagnostic_trace.h"

#include <assert.h>
#include <string.h>

static void test_peer_record(void)
{
	uint8_t record[VITA_NS_PEER_RECORD_SIZE];
	uint32_t low = 0;
	uint16_t high = 0;
	vita_ns_encode_peer_record(record, 0x44332211U, 0x6655U);
	const uint8_t expected[VITA_NS_PEER_RECORD_SIZE] = {
		0x56, 0x4e, 0x53, 0x01, 0x11, 0x22, 0x33, 0x44,
		0x55, 0x66, 0x98, 0x67,
	};
	assert(!memcmp(record, expected, sizeof(expected)));
	assert(vita_ns_decode_peer_record(record, &low, &high) == 0);
	assert(low == 0x44332211U && high == 0x6655U);
	record[5] ^= 1;
	assert(vita_ns_decode_peer_record(record, &low, &high) == -1);
	memset(record, 0, sizeof(record));
	vita_ns_encode_peer_record(record, 0, 0);
	assert(vita_ns_decode_peer_record(record, &low, &high) == -2);
}

static void test_link_key_record(void)
{
	const uint8_t original_key[VITA_NS_LINK_KEY_SIZE] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
	};
	uint8_t decoded_key[VITA_NS_LINK_KEY_SIZE];
	uint8_t record[VITA_NS_LINK_KEY_RECORD_SIZE];
	uint32_t low = 0;
	uint16_t high = 0;
	uint8_t key_type = 0;
	vita_ns_encode_link_key_record(record, 0x44332211U, 0x6655U,
		original_key, 4);
	const uint8_t expected[VITA_NS_LINK_KEY_RECORD_SIZE] = {
		0x56, 0x4e, 0x4b, 0x01, 0x11, 0x22, 0x33, 0x44,
		0x55, 0x66, 0x04, 0x00, 0x00, 0x01, 0x02, 0x03,
		0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
		0x0c, 0x0d, 0x0e, 0x0f, 0xca, 0xbb, 0x1c, 0x52,
	};
	assert(!memcmp(record, expected, sizeof(expected)));
	assert(vita_ns_decode_link_key_record(record, &low, &high,
		decoded_key, &key_type) == 0);
	assert(low == 0x44332211U && high == 0x6655U && key_type == 4);
	assert(!memcmp(original_key, decoded_key, sizeof(original_key)));
	record[12] ^= 1;
	assert(vita_ns_decode_link_key_record(record, &low, &high,
		decoded_key, &key_type) == -1);
}

static void test_diagnostic_trace(void)
{
	const uint8_t payload[] = { 1, 2, 3 };
	diagnostic_trace_reset();
	diagnostic_trace_publish(9, payload, sizeof(payload));
	const DiagnosticTraceEvent *event = diagnostic_trace_event(0);
	assert(event->sequence == 1 && event->type == 9);
	assert(event->length == sizeof(payload));
	assert(!memcmp(event->data, payload, sizeof(payload)));
	for (unsigned int i = 1; i <= DIAGNOSTIC_TRACE_CAPACITY; i++)
		diagnostic_trace_publish(10, NULL, 0);
	event = diagnostic_trace_event(0);
	assert(event->sequence == DIAGNOSTIC_TRACE_CAPACITY + 1);
}

static void test_bond_state(void)
{
	const uint8_t key[VITA_NS_LINK_KEY_SIZE] = { 1, 2, 3, 4 };
	uint8_t read_key[VITA_NS_LINK_KEY_SIZE];
	uint32_t low;
	uint16_t high;
	uint8_t key_type;
	unsigned int generation;
	bond_state_reset();
	assert(!bond_state_read_peer(&low, &high));
	bond_state_set_peer(0x44332211U, 0x6655U);
	assert(bond_state_peer_matches(0x44332211U, 0x6655U));
	bond_state_set_link_key(0x44332211U, 0x6655U, key, 4);
	assert(bond_state_has_saved_bond());
	assert(bond_state_read_link_key_for_peer(0x44332211U, 0x6655U,
		read_key, &key_type));
	assert(key_type == 4 && !memcmp(key, read_key, sizeof(key)));
	bond_state_queue_link_key(0x88776655U, 0xaa99U, key, 5);
	assert(bond_state_read_pending_link_key(&low, &high, read_key,
		&key_type, &generation));
	assert(low == 0x88776655U && high == 0xaa99U && key_type == 5);
	assert(generation == 2);
	bond_state_clear_link_key();
	assert(!bond_state_has_saved_bond());
}

int main(void)
{
	test_peer_record();
	test_link_key_record();
	test_diagnostic_trace();
	test_bond_state();
	return 0;
}
