#ifndef VITA_NS_BOND_RECORD_H
#define VITA_NS_BOND_RECORD_H

#include <stdint.h>

#define VITA_NS_PEER_RECORD_SIZE 12U
#define VITA_NS_LINK_KEY_RECORD_SIZE 32U
#define VITA_NS_LINK_KEY_SIZE 16U

int vita_ns_valid_peer_address(uint32_t low, uint16_t high);
void vita_ns_encode_peer_record(uint8_t record[VITA_NS_PEER_RECORD_SIZE],
	uint32_t low, uint16_t high);
int vita_ns_decode_peer_record(
	const uint8_t record[VITA_NS_PEER_RECORD_SIZE],
	uint32_t *low, uint16_t *high);
void vita_ns_encode_link_key_record(
	uint8_t record[VITA_NS_LINK_KEY_RECORD_SIZE], uint32_t low,
	uint16_t high, const uint8_t key[VITA_NS_LINK_KEY_SIZE],
	uint8_t key_type);
int vita_ns_decode_link_key_record(
	const uint8_t record[VITA_NS_LINK_KEY_RECORD_SIZE], uint32_t *low,
	uint16_t *high, uint8_t key[VITA_NS_LINK_KEY_SIZE],
	uint8_t *key_type);

#endif
