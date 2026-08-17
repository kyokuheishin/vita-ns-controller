#ifndef VITA_NS_BOND_STATE_H
#define VITA_NS_BOND_STATE_H

#include <stdint.h>

#include "bond_record.h"

void bond_state_reset(void);
int bond_state_read_peer(uint32_t *low, uint16_t *high);
int bond_state_peer_matches(uint32_t low, uint16_t high);
void bond_state_set_peer(uint32_t low, uint16_t high);
void bond_state_set_link_key(uint32_t low, uint16_t high,
	const uint8_t key[VITA_NS_LINK_KEY_SIZE], uint8_t key_type);
void bond_state_clear_link_key(void);
void bond_state_queue_link_key(uint32_t low, uint16_t high,
	const uint8_t key[VITA_NS_LINK_KEY_SIZE], uint8_t key_type);
int bond_state_read_pending_link_key(uint32_t *low, uint16_t *high,
	uint8_t key[VITA_NS_LINK_KEY_SIZE], uint8_t *key_type,
	unsigned int *generation);
int bond_state_read_link_key(uint32_t *low, uint16_t *high,
	uint8_t key[VITA_NS_LINK_KEY_SIZE], uint8_t *key_type);
int bond_state_read_link_key_for_peer(uint32_t low, uint16_t high,
	uint8_t key[VITA_NS_LINK_KEY_SIZE], uint8_t *key_type);
int bond_state_has_saved_bond(void);

#endif
