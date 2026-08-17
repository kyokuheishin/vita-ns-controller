#include "bond_state.h"

#include <string.h>

#define SNAPSHOT_READ_ATTEMPTS 4

typedef struct {
	volatile unsigned int sequence;
	uint32_t low;
	uint16_t high;
	uint8_t key[VITA_NS_LINK_KEY_SIZE];
	uint8_t key_type;
	volatile int valid;
} LinkKeySnapshot;

static volatile unsigned int peer_sequence;
static uint32_t peer_low;
static uint16_t peer_high;
static volatile int peer_valid;
static LinkKeySnapshot saved_key;
static LinkKeySnapshot pending_key;

void bond_state_reset(void)
{
	peer_sequence = 0;
	peer_low = 0;
	peer_high = 0;
	peer_valid = 0;
	memset(&saved_key, 0, sizeof(saved_key));
	memset(&pending_key, 0, sizeof(pending_key));
}

int bond_state_read_peer(uint32_t *low, uint16_t *high)
{
	for (int attempt = 0; attempt < SNAPSHOT_READ_ATTEMPTS; attempt++) {
		unsigned int before = peer_sequence;
		if (before & 1U)
			continue;
		__sync_synchronize();
		uint32_t read_low = peer_low;
		uint16_t read_high = peer_high;
		int read_valid = peer_valid;
		__sync_synchronize();
		if (before == peer_sequence && read_valid &&
		    vita_ns_valid_peer_address(read_low, read_high)) {
			*low = read_low;
			*high = read_high;
			return 1;
		}
	}
	return 0;
}

int bond_state_peer_matches(uint32_t low, uint16_t high)
{
	uint32_t saved_low;
	uint16_t saved_high;
	return bond_state_read_peer(&saved_low, &saved_high) &&
		saved_low == low && saved_high == high;
}

void bond_state_set_peer(uint32_t low, uint16_t high)
{
	int valid = vita_ns_valid_peer_address(low, high);
	__sync_fetch_and_add(&peer_sequence, 1);
	peer_low = low;
	peer_high = high;
	peer_valid = valid;
	__sync_synchronize();
	__sync_fetch_and_add(&peer_sequence, 1);
}

static void set_key_snapshot(LinkKeySnapshot *snapshot, uint32_t low,
	uint16_t high, const uint8_t key[VITA_NS_LINK_KEY_SIZE],
	uint8_t key_type)
{
	__sync_fetch_and_add(&snapshot->sequence, 1);
	snapshot->low = low;
	snapshot->high = high;
	memcpy(snapshot->key, key, sizeof(snapshot->key));
	snapshot->key_type = key_type;
	snapshot->valid = vita_ns_valid_peer_address(low, high);
	__sync_synchronize();
	__sync_fetch_and_add(&snapshot->sequence, 1);
}

void bond_state_set_link_key(uint32_t low, uint16_t high,
	const uint8_t key[VITA_NS_LINK_KEY_SIZE], uint8_t key_type)
{
	set_key_snapshot(&saved_key, low, high, key, key_type);
}

void bond_state_clear_link_key(void)
{
	uint8_t empty_key[VITA_NS_LINK_KEY_SIZE] = { 0 };
	set_key_snapshot(&saved_key, 0, 0, empty_key, 0);
}

void bond_state_queue_link_key(uint32_t low, uint16_t high,
	const uint8_t key[VITA_NS_LINK_KEY_SIZE], uint8_t key_type)
{
	set_key_snapshot(&pending_key, low, high, key, key_type);
}

static int read_key_snapshot(const LinkKeySnapshot *snapshot,
	uint32_t *low, uint16_t *high, uint8_t key[VITA_NS_LINK_KEY_SIZE],
	uint8_t *key_type, unsigned int *generation)
{
	for (int attempt = 0; attempt < SNAPSHOT_READ_ATTEMPTS; attempt++) {
		unsigned int before = snapshot->sequence;
		if (before & 1U)
			continue;
		__sync_synchronize();
		uint32_t read_low = snapshot->low;
		uint16_t read_high = snapshot->high;
		uint8_t read_key[VITA_NS_LINK_KEY_SIZE];
		memcpy(read_key, snapshot->key, sizeof(read_key));
		uint8_t read_type = snapshot->key_type;
		int read_valid = snapshot->valid;
		__sync_synchronize();
		if (before == snapshot->sequence && read_valid &&
		    vita_ns_valid_peer_address(read_low, read_high)) {
			*low = read_low;
			*high = read_high;
			memcpy(key, read_key, sizeof(read_key));
			*key_type = read_type;
			if (generation)
				*generation = before;
			memset(read_key, 0, sizeof(read_key));
			return 1;
		}
		memset(read_key, 0, sizeof(read_key));
	}
	return 0;
}

int bond_state_read_pending_link_key(uint32_t *low, uint16_t *high,
	uint8_t key[VITA_NS_LINK_KEY_SIZE], uint8_t *key_type,
	unsigned int *generation)
{
	return read_key_snapshot(&pending_key, low, high, key, key_type,
		generation);
}

int bond_state_read_link_key(uint32_t *low, uint16_t *high,
	uint8_t key[VITA_NS_LINK_KEY_SIZE], uint8_t *key_type)
{
	return read_key_snapshot(&saved_key, low, high, key, key_type, NULL);
}

int bond_state_read_link_key_for_peer(uint32_t low, uint16_t high,
	uint8_t key[VITA_NS_LINK_KEY_SIZE], uint8_t *key_type)
{
	uint32_t saved_low;
	uint16_t saved_high;
	if (!bond_state_read_link_key(&saved_low, &saved_high, key, key_type))
		return 0;
	if (saved_low == low && saved_high == high)
		return 1;
	memset(key, 0, VITA_NS_LINK_KEY_SIZE);
	return 0;
}

int bond_state_has_saved_bond(void)
{
	uint32_t low;
	uint16_t high;
	uint8_t key[VITA_NS_LINK_KEY_SIZE];
	uint8_t key_type = 0;
	if (!bond_state_read_peer(&low, &high))
		return 0;
	int valid = bond_state_read_link_key_for_peer(low, high, key,
		&key_type);
	memset(key, 0, sizeof(key));
	return valid;
}
