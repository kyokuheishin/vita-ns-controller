#include "bond_record.h"

#include <string.h>

#define PEER_RECORD_VERSION 1U
#define LINK_KEY_RECORD_VERSION 1U
#define PEER_CHECKSUM_SEED 0xa5U
#define FNV1A_OFFSET_BASIS 2166136261U
#define FNV1A_PRIME 16777619U

int vita_ns_valid_peer_address(uint32_t low, uint16_t high)
{
	return (low || high) && !(low == UINT32_MAX && high == UINT16_MAX);
}

static uint8_t peer_record_checksum(
	const uint8_t record[VITA_NS_PEER_RECORD_SIZE])
{
	uint8_t checksum = PEER_CHECKSUM_SEED;
	for (unsigned int i = 0; i < 10; i++)
		checksum ^= record[i];
	return checksum;
}

void vita_ns_encode_peer_record(uint8_t record[VITA_NS_PEER_RECORD_SIZE],
	uint32_t low, uint16_t high)
{
	record[0] = 'V';
	record[1] = 'N';
	record[2] = 'S';
	record[3] = PEER_RECORD_VERSION;
	record[4] = (uint8_t)low;
	record[5] = (uint8_t)(low >> 8);
	record[6] = (uint8_t)(low >> 16);
	record[7] = (uint8_t)(low >> 24);
	record[8] = (uint8_t)high;
	record[9] = (uint8_t)(high >> 8);
	record[10] = peer_record_checksum(record);
	record[11] = (uint8_t)~record[10];
}

int vita_ns_decode_peer_record(
	const uint8_t record[VITA_NS_PEER_RECORD_SIZE],
	uint32_t *low, uint16_t *high)
{
	if (record[0] != 'V' || record[1] != 'N' || record[2] != 'S' ||
	    record[3] != PEER_RECORD_VERSION ||
	    record[10] != peer_record_checksum(record) ||
	    record[11] != (uint8_t)~record[10])
		return -1;
	uint32_t decoded_low = record[4] | ((uint32_t)record[5] << 8) |
		((uint32_t)record[6] << 16) | ((uint32_t)record[7] << 24);
	uint16_t decoded_high = record[8] | ((uint16_t)record[9] << 8);
	if (!vita_ns_valid_peer_address(decoded_low, decoded_high))
		return -2;
	*low = decoded_low;
	*high = decoded_high;
	return 0;
}

static uint32_t link_key_record_checksum(
	const uint8_t record[VITA_NS_LINK_KEY_RECORD_SIZE])
{
	uint32_t checksum = FNV1A_OFFSET_BASIS;
	for (unsigned int i = 0; i < VITA_NS_LINK_KEY_RECORD_SIZE - 4; i++) {
		checksum ^= record[i];
		checksum *= FNV1A_PRIME;
	}
	return checksum;
}

void vita_ns_encode_link_key_record(
	uint8_t record[VITA_NS_LINK_KEY_RECORD_SIZE], uint32_t low,
	uint16_t high, const uint8_t key[VITA_NS_LINK_KEY_SIZE],
	uint8_t key_type)
{
	record[0] = 'V';
	record[1] = 'N';
	record[2] = 'K';
	record[3] = LINK_KEY_RECORD_VERSION;
	record[4] = (uint8_t)low;
	record[5] = (uint8_t)(low >> 8);
	record[6] = (uint8_t)(low >> 16);
	record[7] = (uint8_t)(low >> 24);
	record[8] = (uint8_t)high;
	record[9] = (uint8_t)(high >> 8);
	record[10] = key_type;
	record[11] = 0;
	memcpy(record + 12, key, VITA_NS_LINK_KEY_SIZE);
	uint32_t checksum = link_key_record_checksum(record);
	record[28] = (uint8_t)checksum;
	record[29] = (uint8_t)(checksum >> 8);
	record[30] = (uint8_t)(checksum >> 16);
	record[31] = (uint8_t)(checksum >> 24);
}

int vita_ns_decode_link_key_record(
	const uint8_t record[VITA_NS_LINK_KEY_RECORD_SIZE], uint32_t *low,
	uint16_t *high, uint8_t key[VITA_NS_LINK_KEY_SIZE],
	uint8_t *key_type)
{
	uint32_t stored_checksum = record[28] |
		((uint32_t)record[29] << 8) |
		((uint32_t)record[30] << 16) |
		((uint32_t)record[31] << 24);
	if (record[0] != 'V' || record[1] != 'N' || record[2] != 'K' ||
	    record[3] != LINK_KEY_RECORD_VERSION || record[11] != 0 ||
	    stored_checksum != link_key_record_checksum(record))
		return -1;
	uint32_t decoded_low = record[4] | ((uint32_t)record[5] << 8) |
		((uint32_t)record[6] << 16) | ((uint32_t)record[7] << 24);
	uint16_t decoded_high = record[8] | ((uint16_t)record[9] << 8);
	if (!vita_ns_valid_peer_address(decoded_low, decoded_high))
		return -2;
	*low = decoded_low;
	*high = decoded_high;
	memcpy(key, record + 12, VITA_NS_LINK_KEY_SIZE);
	*key_type = record[10];
	return 0;
}
