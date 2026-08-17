#include "procon.h"
#include "raw_l2cap.h"
#include "touch_map.h"

#include <assert.h>
#include <string.h>

static void test_raw_l2cap_packets(void)
{
	uint8_t connection[RAW_L2CAP_CONNECTION_PACKET_SIZE];
	prepare_l2cap_signaling_packet(connection, sizeof(connection), 1,
		L2CAP_SIGNAL_CONNECTION_REQUEST, 0x50,
		L2CAP_CONNECTION_REQUEST_DATA_SIZE);
	write_le16(connection + L2CAP_SIGNALING_DATA_OFFSET,
		L2CAP_PSM_HID_CONTROL);
	write_le16(connection + L2CAP_SIGNALING_DATA_OFFSET + 2,
		L2CAP_LOCAL_CID_HID_CONTROL);
	static const uint8_t expected_connection[] = {
		0x01, 0x20, 0x0c, 0x00, 0x08, 0x00, 0x01, 0x00,
		0x02, 0x50, 0x04, 0x00, 0x11, 0x00, 0x40, 0x00,
	};
	assert(sizeof(connection) == sizeof(expected_connection));
	assert(!memcmp(connection, expected_connection, sizeof(connection)));

	uint8_t response[RAW_L2CAP_CONFIGURATION_RESPONSE_PACKET_SIZE];
	uint8_t request[RAW_L2CAP_CONFIGURATION_REQUEST_PACKET_SIZE];
	prepare_l2cap_configuration_packets(response, sizeof(response),
		request, sizeof(request), 1, 0x0042, 0x2f, 0x60);
	static const uint8_t expected_response[] = {
		0x01, 0x20, 0x12, 0x00, 0x0e, 0x00, 0x01, 0x00,
		0x05, 0x2f, 0x0a, 0x00, 0x42, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x01, 0x02, 0xa0, 0x02,
	};
	static const uint8_t expected_request[] = {
		0x01, 0x20, 0x10, 0x00, 0x0c, 0x00, 0x01, 0x00,
		0x04, 0x60, 0x08, 0x00, 0x42, 0x00, 0x00, 0x00,
		0x01, 0x02, 0xa0, 0x02,
	};
	assert(sizeof(response) == sizeof(expected_response));
	assert(sizeof(request) == sizeof(expected_request));
	assert(!memcmp(response, expected_response, sizeof(response)));
	assert(!memcmp(request, expected_request, sizeof(request)));

	uint8_t input[RAW_ACL_HEADER_SIZE + PROCON_INPUT_SIZE];
	prepare_acl_l2cap_packet(input, sizeof(input), 1, 0x0043,
		PROCON_INPUT_SIZE);
	static const uint8_t expected_input_header[] = {
		0x01, 0x20, 0x36, 0x00, 0x32, 0x00, 0x43, 0x00,
	};
	assert(!memcmp(input, expected_input_header,
		sizeof(expected_input_header)));
	assert(read_le16(input + 2) == L2CAP_HEADER_SIZE + PROCON_INPUT_SIZE);
}

static size_t command(ProconState *state, uint8_t id, const uint8_t *data,
		      size_t data_size, uint8_t reply[PROCON_REPLY_SIZE])
{
	uint8_t packet[32] = {0xa2, 0x01};
	packet[11] = id;
	if (data_size)
		memcpy(packet + 12, data, data_size);
	return procon_handle_output(state, packet, 12 + data_size, 7, reply);
}

static void expect_ack(ProconState *state, uint8_t id, uint8_t ack,
		       uint8_t reply[PROCON_REPLY_SIZE])
{
	assert(command(state, id, NULL, 0, reply) == PROCON_REPLY_SIZE);
	assert(reply[0] == 0xa1 && reply[1] == 0x21);
	assert(reply[14] == ack && reply[15] == id);
}

int main(void)
{
	test_raw_l2cap_packets();
	assert(vita_ns_touch_button(0, 543) == 0);
	assert(vita_ns_touch_button(0, 544) == VITA_NS_TOUCH_ZL);
	assert(vita_ns_touch_button(479, 815) == VITA_NS_TOUCH_ZL);
	assert(vita_ns_touch_button(0, 816) == VITA_NS_TOUCH_L3);
	assert(vita_ns_touch_button(479, 1087) == VITA_NS_TOUCH_L3);
	assert(vita_ns_touch_button(480, 544) == VITA_NS_TOUCH_CAPTURE);
	assert(vita_ns_touch_button(959, 1087) == VITA_NS_TOUCH_CAPTURE);
	assert(vita_ns_touch_button(960, 544) == VITA_NS_TOUCH_HOME);
	assert(vita_ns_touch_button(1439, 1087) == VITA_NS_TOUCH_HOME);
	assert(vita_ns_touch_button(1440, 544) == VITA_NS_TOUCH_ZR);
	assert(vita_ns_touch_button(1919, 815) == VITA_NS_TOUCH_ZR);
	assert(vita_ns_touch_button(1440, 816) == VITA_NS_TOUCH_R3);
	assert(vita_ns_touch_button(1919, 1087) == VITA_NS_TOUCH_R3);
	assert(vita_ns_touch_button(1920, 800) == 0);
	assert(vita_ns_touch_button_for_layout(0, 0,
		VITA_NS_TOUCH_LAYOUT_FULL) == VITA_NS_TOUCH_ZL);
	assert(vita_ns_touch_button_for_layout(559, 479,
		VITA_NS_TOUCH_LAYOUT_FULL) == VITA_NS_TOUCH_ZL);
	assert(vita_ns_touch_button_for_layout(560, 0,
		VITA_NS_TOUCH_LAYOUT_FULL) == 0);
	assert(vita_ns_touch_button_for_layout(0, 607,
		VITA_NS_TOUCH_LAYOUT_FULL) == 0);
	assert(vita_ns_touch_button_for_layout(0, 608,
		VITA_NS_TOUCH_LAYOUT_FULL) == VITA_NS_TOUCH_L3);
	assert(vita_ns_touch_button_for_layout(1359, 0,
		VITA_NS_TOUCH_LAYOUT_FULL) == 0);
	assert(vita_ns_touch_button_for_layout(620, 688,
		VITA_NS_TOUCH_LAYOUT_FULL) == VITA_NS_TOUCH_CAPTURE);
	assert(vita_ns_touch_button_for_layout(939, 1007,
		VITA_NS_TOUCH_LAYOUT_FULL) == VITA_NS_TOUCH_CAPTURE);
	assert(vita_ns_touch_button_for_layout(940, 1007,
		VITA_NS_TOUCH_LAYOUT_FULL) == 0);
	assert(vita_ns_touch_button_for_layout(980, 688,
		VITA_NS_TOUCH_LAYOUT_FULL) == VITA_NS_TOUCH_HOME);
	assert(vita_ns_touch_button_for_layout(1299, 1007,
		VITA_NS_TOUCH_LAYOUT_FULL) == VITA_NS_TOUCH_HOME);
	assert(vita_ns_touch_button_for_layout(1360, 0,
		VITA_NS_TOUCH_LAYOUT_FULL) == VITA_NS_TOUCH_ZR);
	assert(vita_ns_touch_button_for_layout(1919, 479,
		VITA_NS_TOUCH_LAYOUT_FULL) == VITA_NS_TOUCH_ZR);
	assert(vita_ns_touch_button_for_layout(1360, 608,
		VITA_NS_TOUCH_LAYOUT_FULL) == VITA_NS_TOUCH_R3);
	assert(vita_ns_back_touch_button(0, 0) == VITA_NS_TOUCH_BACK_LEFT);
	assert(vita_ns_back_touch_button(959, 1087) == VITA_NS_TOUCH_BACK_LEFT);
	assert(vita_ns_back_touch_button(960, 0) == VITA_NS_TOUCH_BACK_RIGHT);
	assert(vita_ns_back_touch_button(1919, 1087) == VITA_NS_TOUCH_BACK_RIGHT);
	assert(vita_ns_back_touch_button(1920, 500) == 0);
	const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
	ProconState state;
	uint8_t reply[PROCON_REPLY_SIZE];
	procon_init(&state, mac);
	assert(procon_hid_report_descriptor_size == 170);
	assert(memcmp(procon_hid_report_descriptor,
		      "\x05\x01\x09\x05\xa1\x01", 6) == 0);
	assert(procon_hid_report_descriptor[169] == 0xc0);

	expect_ack(&state, 0x01, 0x81, reply);
	assert(reply[16] == 0x03);

	assert(command(&state, 0x02, NULL, 0, reply) == PROCON_REPLY_SIZE);
	assert(reply[0] == 0xa1 && reply[1] == 0x21 && reply[14] == 0x82);
	assert(reply[15] == 0x02 && reply[18] == 0x03);
	assert(memcmp(reply + 20, mac, sizeof(mac)) == 0);

	const uint8_t mode[] = {0x30};
	assert(command(&state, 0x03, mode, sizeof(mode), reply) == PROCON_REPLY_SIZE);
	assert(state.input_mode == 0x30 && reply[14] == 0x80);

	expect_ack(&state, 0x04, 0x83, reply);
	assert(reply[16] == 0x2c && reply[17] == 0x01);
	expect_ack(&state, 0x08, 0x80, reply);

	const uint8_t spi[] = {0x3d, 0x60, 0x00, 0x00, 9};
	assert(command(&state, 0x10, spi, sizeof(spi), reply) == PROCON_REPLY_SIZE);
	assert(reply[14] == 0x90 && reply[20] == 9);
	assert(memcmp(reply + 21, "\xff\xf7\x7f\x00\x08\x80\x00\x08\x80", 9) == 0);

	const uint8_t spi_overlap[] = {0xff, 0x5f, 0x00, 0x00, 18};
	assert(command(&state, 0x10, spi_overlap, sizeof(spi_overlap), reply) ==
	       PROCON_REPLY_SIZE);
	assert(reply[21] == 0x00 && reply[37] == 0x00 && reply[38] == 0xff);
	const uint8_t spi_6000[] = {0x00, 0x60, 0x00, 0x00, 16};
	assert(command(&state, 0x10, spi_6000, sizeof(spi_6000), reply) ==
	       PROCON_REPLY_SIZE);
	for (int i = 0; i < 16; i++)
		assert(reply[21 + i] == 0x00);
	const uint8_t spi_6080[] = {0x80, 0x60, 0x00, 0x00, 24};
	assert(command(&state, 0x10, spi_6080, sizeof(spi_6080), reply) ==
	       PROCON_REPLY_SIZE);
	for (int i = 0; i < 24; i++)
		assert(reply[21 + i] == 0x00);
	const uint8_t spi_zero[] = {0xff, 0xff, 0xff, 0xff, 0};
	assert(command(&state, 0x10, spi_zero, sizeof(spi_zero), reply) ==
	       PROCON_REPLY_SIZE);
	assert(reply[14] == 0x90 && reply[20] == 0);
	const uint8_t spi_large[] = {0x00, 0x60, 0x00, 0x00, 0x1e};
	assert(command(&state, 0x10, spi_large, sizeof(spi_large), reply) ==
	       PROCON_REPLY_SIZE);
	assert(reply[14] == 0x83);

	expect_ack(&state, 0x21, 0xa0, reply);
	assert(memcmp(reply + 16, "\x01\x00\xff\x00\x08\x00\x1b\x01", 8) == 0);
	assert(reply[49] == 0xc8);
	expect_ack(&state, 0x22, 0x80, reply);
	expect_ack(&state, 0x38, 0x80, reply);
	expect_ack(&state, 0x41, 0x80, reply);

	const uint8_t lights[] = {0xa5};
	assert(command(&state, 0x30, lights, sizeof(lights), reply) ==
	       PROCON_REPLY_SIZE);
	assert(state.player_lights == 0xa5);
	const uint8_t enabled[] = {1};
	assert(command(&state, 0x40, enabled, sizeof(enabled), reply) ==
	       PROCON_REPLY_SIZE);
	assert(reply[14] == 0x80);
	assert(command(&state, 0x48, enabled, sizeof(enabled), reply) ==
	       PROCON_REPLY_SIZE);
	assert(state.vibration_enabled == 1);

	const uint8_t buttons[3] = {0x81, 0x42, 0x24};
	uint8_t input[PROCON_INPUT_SIZE];
	procon_set_input(&state, buttons, 0x123, 0x456, 0x789, 0xabc);
	assert(procon_make_input(&state, 9, input) == PROCON_INPUT_SIZE);
	assert(input[0] == 0xa1 && input[1] == 0x30 && input[2] == 9);
	assert(input[3] == 0x80);
	assert(memcmp(input + 4, buttons, sizeof(buttons)) == 0);
	assert(memcmp(input + 7, "\x23\x61\x45\x89\xc7\xab", 6) == 0);
	procon_set_battery(&state, 74, 0);
	procon_make_input(&state, 10, input);
	assert(input[3] == 0x60);
	procon_set_battery(&state, 53, 0);
	procon_make_input(&state, 11, input);
	assert(input[3] == 0x40);
	procon_set_battery(&state, 24, 1);
	procon_make_input(&state, 12, input);
	assert(input[3] == 0x30);
	procon_set_battery(&state, 100, 1);
	procon_make_input(&state, 13, input);
	assert(input[3] == 0x90);
	procon_make_input(&state, 14, input);
	for (int i = 14; i < PROCON_INPUT_SIZE; i++)
		assert(input[i] == 0);

	assert(procon_handle_output(&state, NULL, 99, 0, reply) == 0);
	assert(procon_handle_output(&state, (const uint8_t *)"bad", 3, 0, reply) == 0);
	uint8_t bad_packet[17] = {0xa2, 0x01};
	bad_packet[11] = 0xff;
	assert(procon_handle_output(&state, bad_packet, sizeof(bad_packet), 0,
				    reply) == 0);
	bad_packet[11] = 0x03;
	assert(procon_handle_output(&state, bad_packet, 12, 0, reply) == 0);
	bad_packet[11] = 0x10;
	assert(procon_handle_output(&state, bad_packet, 16, 0, reply) == 0);
	bad_packet[0] = 0xa1;
	assert(procon_handle_output(&state, bad_packet, sizeof(bad_packet), 0,
				    reply) == 0);
	return 0;
}
