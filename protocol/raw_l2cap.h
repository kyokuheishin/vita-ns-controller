#ifndef RAW_L2CAP_H
#define RAW_L2CAP_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	HCI_PACKET_TYPE_ACL_DATA = 0x02,
	HCI_ACL_HANDLE_MASK = 0x0fff,
	HCI_ACL_PB_MASK = 0x3000,
	HCI_ACL_PB_FIRST_NON_FLUSHABLE = 0x2000,
};

enum {
	L2CAP_HEADER_SIZE = 4,
	L2CAP_SIGNALING_HEADER_SIZE = 4,
	RAW_ACL_HEADER_SIZE = 8,
	L2CAP_SIGNALING_COMMAND_OFFSET = RAW_ACL_HEADER_SIZE,
	L2CAP_SIGNALING_IDENTIFIER_OFFSET = RAW_ACL_HEADER_SIZE + 1,
	L2CAP_SIGNALING_LENGTH_OFFSET = RAW_ACL_HEADER_SIZE + 2,
	L2CAP_SIGNALING_DATA_OFFSET =
		RAW_ACL_HEADER_SIZE + L2CAP_SIGNALING_HEADER_SIZE,
	L2CAP_SIGNALING_CID = 0x0001,
	L2CAP_PSM_HID_CONTROL = 0x0011,
	L2CAP_PSM_HID_INTERRUPT = 0x0013,
	L2CAP_FIRST_DYNAMIC_CID = 0x0040,
	L2CAP_LOCAL_CID_HID_CONTROL = 0x0040,
	L2CAP_LOCAL_CID_HID_INTERRUPT = 0x0041,
	L2CAP_SIGNAL_CONNECTION_REQUEST = 0x02,
	L2CAP_SIGNAL_CONNECTION_RESPONSE = 0x03,
	L2CAP_SIGNAL_CONFIGURATION_REQUEST = 0x04,
	L2CAP_SIGNAL_CONFIGURATION_RESPONSE = 0x05,
	L2CAP_CONNECTION_REQUEST_DATA_SIZE = 4,
	L2CAP_CONFIGURATION_REQUEST_DATA_SIZE = 8,
	L2CAP_CONFIGURATION_RESPONSE_DATA_SIZE = 10,
	RAW_L2CAP_CONNECTION_PACKET_SIZE =
		L2CAP_SIGNALING_DATA_OFFSET + L2CAP_CONNECTION_REQUEST_DATA_SIZE,
	RAW_L2CAP_CONFIGURATION_REQUEST_PACKET_SIZE =
		L2CAP_SIGNALING_DATA_OFFSET + L2CAP_CONFIGURATION_REQUEST_DATA_SIZE,
	RAW_L2CAP_CONFIGURATION_RESPONSE_PACKET_SIZE =
		L2CAP_SIGNALING_DATA_OFFSET + L2CAP_CONFIGURATION_RESPONSE_DATA_SIZE,
	L2CAP_CONFIGURATION_SUCCESS = 0x0000,
	L2CAP_CONFIGURATION_OPTION_MTU = 0x01,
	L2CAP_HID_MTU = 0x02a0,
};

static inline void write_le16(uint8_t *destination, uint16_t value)
{
	destination[0] = (uint8_t)value;
	destination[1] = (uint8_t)(value >> 8);
}

static inline uint16_t read_le16(const uint8_t *source)
{
	return (uint16_t)(source[0] | ((uint16_t)source[1] << 8));
}

static inline void prepare_acl_l2cap_packet(uint8_t *packet,
	size_t packet_size, uint16_t handle, uint16_t cid, uint16_t payload_size)
{
	memset(packet, 0, packet_size);
	write_le16(packet, (uint16_t)((handle & HCI_ACL_HANDLE_MASK) |
		HCI_ACL_PB_FIRST_NON_FLUSHABLE));
	write_le16(packet + 2, (uint16_t)(L2CAP_HEADER_SIZE + payload_size));
	write_le16(packet + 4, payload_size);
	write_le16(packet + 6, cid);
}

static inline void prepare_l2cap_signaling_packet(uint8_t *packet,
	size_t packet_size, uint16_t handle, uint8_t command,
	uint8_t identifier, uint16_t payload_size)
{
	prepare_acl_l2cap_packet(packet, packet_size, handle,
		L2CAP_SIGNALING_CID,
		(uint16_t)(L2CAP_SIGNALING_HEADER_SIZE + payload_size));
	packet[L2CAP_SIGNALING_COMMAND_OFFSET] = command;
	packet[L2CAP_SIGNALING_IDENTIFIER_OFFSET] = identifier;
	write_le16(packet + L2CAP_SIGNALING_LENGTH_OFFSET, payload_size);
}

static inline void prepare_l2cap_configuration_packets(
	uint8_t *response, size_t response_size,
	uint8_t *request, size_t request_size,
	uint16_t handle, uint16_t remote_cid,
	uint8_t response_identifier, uint8_t request_identifier)
{
	prepare_l2cap_signaling_packet(response, response_size, handle,
		L2CAP_SIGNAL_CONFIGURATION_RESPONSE, response_identifier,
		L2CAP_CONFIGURATION_RESPONSE_DATA_SIZE);
	write_le16(response + L2CAP_SIGNALING_DATA_OFFSET, remote_cid);
	write_le16(response + L2CAP_SIGNALING_DATA_OFFSET + 2, 0); /* Flags */
	write_le16(response + L2CAP_SIGNALING_DATA_OFFSET + 4,
		L2CAP_CONFIGURATION_SUCCESS);
	response[L2CAP_SIGNALING_DATA_OFFSET + 6] =
		L2CAP_CONFIGURATION_OPTION_MTU;
	response[L2CAP_SIGNALING_DATA_OFFSET + 7] = sizeof(uint16_t);
	write_le16(response + L2CAP_SIGNALING_DATA_OFFSET + 8, L2CAP_HID_MTU);

	prepare_l2cap_signaling_packet(request, request_size, handle,
		L2CAP_SIGNAL_CONFIGURATION_REQUEST, request_identifier,
		L2CAP_CONFIGURATION_REQUEST_DATA_SIZE);
	write_le16(request + L2CAP_SIGNALING_DATA_OFFSET, remote_cid);
	write_le16(request + L2CAP_SIGNALING_DATA_OFFSET + 2, 0); /* Flags */
	request[L2CAP_SIGNALING_DATA_OFFSET + 4] =
		L2CAP_CONFIGURATION_OPTION_MTU;
	request[L2CAP_SIGNALING_DATA_OFFSET + 5] = sizeof(uint16_t);
	write_le16(request + L2CAP_SIGNALING_DATA_OFFSET + 6, L2CAP_HID_MTU);
}

#endif
