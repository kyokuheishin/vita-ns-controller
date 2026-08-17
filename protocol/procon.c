#include "procon.h"

#include <string.h>

/*
 * Protocol facts and calibration constants are based on dekuNukem's Switch
 * reverse-engineering notes and the MIT-licensed esp-cpp implementation:
 * https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering
 * https://github.com/finger563/esp-usb-ble-hid
 * Copyright (c) 2023 esp-cpp, used under the MIT License.
 */

/* From the MIT-licensed retro-pico-switch; see THIRD_PARTY_NOTICES.md. */
const uint8_t procon_hid_report_descriptor[] = {
	0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x06, 0x01, 0xff,
	0x85, 0x21, 0x09, 0x21, 0x75, 0x08, 0x95, 0x30, 0x81, 0x02,
	0x85, 0x30, 0x09, 0x30, 0x75, 0x08, 0x95, 0x30, 0x81, 0x02,
	0x85, 0x31, 0x09, 0x31, 0x75, 0x08, 0x96, 0x69, 0x01, 0x81, 0x02,
	0x85, 0x32, 0x09, 0x32, 0x75, 0x08, 0x96, 0x69, 0x01, 0x81, 0x02,
	0x85, 0x33, 0x09, 0x33, 0x75, 0x08, 0x96, 0x69, 0x01, 0x81, 0x02,
	0x85, 0x3f, 0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00,
	0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02, 0x05, 0x01,
	0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01,
	0x81, 0x42, 0x05, 0x09, 0x75, 0x04, 0x95, 0x01, 0x81, 0x01,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34,
	0x16, 0x00, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00, 0x75, 0x10,
	0x95, 0x04, 0x81, 0x02, 0x06, 0x01, 0xff,
	0x85, 0x01, 0x09, 0x01, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02,
	0x85, 0x10, 0x09, 0x10, 0x75, 0x08, 0x95, 0x09, 0x91, 0x02,
	0x85, 0x11, 0x09, 0x11, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02,
	0x85, 0x12, 0x09, 0x12, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02,
	0xc0,
};
const size_t procon_hid_report_descriptor_size =
	sizeof(procon_hid_report_descriptor);

static const uint8_t factory_identity[] = {
	0xff, 0xff, 0x03, 0xa0, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0x02, 0xff, 0xff, 0xff, 0xff,
};
static const uint8_t factory_imu[] = {
	0xf0, 0xff, 0x89, 0x00, 0xf0, 0x01, 0x00, 0x40,
	0x00, 0x40, 0x00, 0x40, 0xf9, 0xff, 0x06, 0x00,
	0x09, 0x00, 0xe7, 0x3b, 0xe7, 0x3b, 0xe7, 0x3b,
};
static const uint8_t factory_sticks_colors[] = {
	0xff, 0xf7, 0x7f, 0x00, 0x08, 0x80, 0x00, 0x08, 0x80,
	0x00, 0x08, 0x80, 0x00, 0x08, 0x80, 0xff, 0xf7, 0x7f,
	0xff, 0x82, 0x82, 0x82, 0x0f, 0x0f, 0x0f, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff,
};
static const uint8_t user_calibration[] = {
	0xb2, 0xa1, 0xff, 0xf7, 0x7f, 0x00, 0x08, 0x80,
	0x00, 0x00, 0x00, 0xb2, 0xa1, 0xff, 0xf7, 0x7f,
	0x00, 0x08, 0x80, 0x00, 0x00, 0x00, 0xb2, 0xa1,
	0xbe, 0xff, 0x3e, 0x00, 0xf0, 0x01, 0x00, 0x40,
	0x00, 0x40, 0x00, 0x40, 0xfe, 0xff, 0xfe, 0xff,
	0x08, 0x00, 0xe7, 0x3b, 0xe7, 0x3b, 0xe7, 0x3b,
};

enum {
	PROCON_SPI_ADDRESS_SIZE = 4,
	PROCON_SPI_REQUEST_SIZE_OFFSET =
		PROCON_OUTPUT_DATA_OFFSET + PROCON_SPI_ADDRESS_SIZE,
	PROCON_SPI_REPLY_SIZE_OFFSET =
		PROCON_REPLY_DATA_OFFSET + PROCON_SPI_ADDRESS_SIZE,
	PROCON_SPI_REPLY_DATA_OFFSET = PROCON_SPI_REPLY_SIZE_OFFSET + 1,
	PROCON_SPI_MAX_READ = PROCON_REPLY_SIZE - PROCON_SPI_REPLY_DATA_OFFSET,
};

static uint16_t clamp_stick(uint16_t value)
{
	return value > PROCON_STICK_MAX ? PROCON_STICK_MAX : value;
}

static void pack_stick(uint8_t out[3], uint16_t x, uint16_t y)
{
	x = clamp_stick(x);
	y = clamp_stick(y);
	out[0] = x;
	out[1] = (x >> 8) | (y << 4);
	out[2] = y >> 4;
}

static void fill_standard(const ProconState *state, uint8_t timer,
			  uint8_t report_id, uint8_t *out, size_t size)
{
	memset(out, 0, size);
	out[PROCON_INPUT_DATA_TYPE_OFFSET] = PROCON_HID_DATA_INPUT;
	out[PROCON_INPUT_REPORT_ID_OFFSET] = report_id;
	out[PROCON_INPUT_TIMER_OFFSET] = timer;
	/* High nibble: 0/2/4/6/8 battery steps, with bit 0 meaning charging.
	 * Low nibble 0 identifies a Bluetooth Pro Controller. */
	out[PROCON_INPUT_BATTERY_OFFSET] =
		(uint8_t)(state->battery_level << 4);
	memcpy(out + PROCON_INPUT_BUTTONS_OFFSET, state->buttons,
		sizeof(state->buttons));
	pack_stick(out + PROCON_INPUT_LEFT_STICK_OFFSET, state->lx, state->ly);
	pack_stick(out + PROCON_INPUT_RIGHT_STICK_OFFSET, state->rx, state->ry);
	out[PROCON_INPUT_VIBRATOR_OFFSET] = 0x80;
}

static void copy_overlap(uint8_t *out, uint32_t request_address,
			 uint8_t request_size, uint32_t block_address,
			 const uint8_t *block, size_t block_size)
{
	uint32_t begin = request_address > block_address ? request_address : block_address;
	uint32_t request_end = request_address + request_size;
	uint32_t block_end = block_address + block_size;
	uint32_t end = request_end < block_end ? request_end : block_end;

	if (begin < end)
		memcpy(out + begin - request_address, block + begin - block_address, end - begin);
}

static void spi_read(uint8_t *out, uint32_t address, uint8_t size)
{
	/* joycontrol's proven fallback is zero-filled flash.  The Switch starts
	 * pairing with a read at 0x6000 and rejects an all-0xff placeholder. */
	memset(out, 0x00, size);
	copy_overlap(out, address, size, 0x6010, factory_identity, sizeof(factory_identity));
	copy_overlap(out, address, size, 0x6020, factory_imu, sizeof(factory_imu));
	copy_overlap(out, address, size, 0x603d, factory_sticks_colors,
		     sizeof(factory_sticks_colors));
	copy_overlap(out, address, size, 0x8010, user_calibration, sizeof(user_calibration));
}

void procon_init(ProconState *state, const uint8_t mac[6])
{
	memset(state, 0, sizeof(*state));
	memcpy(state->mac, mac, sizeof(state->mac));
	state->lx = state->ly = state->rx = state->ry = PROCON_STICK_CENTER;
	state->input_mode = PROCON_INPUT_REPORT_STANDARD;
	state->battery_level = 8;
}

void procon_set_input(ProconState *state,
		      const uint8_t buttons[PROCON_BUTTON_BYTES],
		      uint16_t lx, uint16_t ly, uint16_t rx, uint16_t ry)
{
	memcpy(state->buttons, buttons, sizeof(state->buttons));
	state->lx = clamp_stick(lx);
	state->ly = clamp_stick(ly);
	state->rx = clamp_stick(rx);
	state->ry = clamp_stick(ry);
}

void procon_set_battery(ProconState *state, int percent, int charging)
{
	uint8_t level;
	/* The protocol exposes five coarse values representing roughly
	 * 0/25/50/75/100%.  Select the nearest value rather than rounding 50%
	 * upward to the 75% icon. */
	if (percent < 13)
		level = 0;
	else if (percent < 38)
		level = 2;
	else if (percent < 63)
		level = 4;
	else if (percent < 88)
		level = 6;
	else
		level = 8;
	state->battery_level = level | (charging ? 1U : 0U);
}

size_t procon_make_input(const ProconState *state, uint8_t timer,
			 uint8_t report[PROCON_INPUT_SIZE])
{
	fill_standard(state, timer, PROCON_INPUT_REPORT_STANDARD, report,
		PROCON_INPUT_SIZE);
	return PROCON_INPUT_SIZE;
}

size_t procon_handle_output(ProconState *state, const uint8_t *packet,
			    size_t packet_size, uint8_t timer,
			    uint8_t reply[PROCON_REPLY_SIZE])
{
	if (!packet || packet_size < PROCON_OUTPUT_DATA_OFFSET ||
	    packet[0] != PROCON_HID_DATA_OUTPUT ||
	    packet[1] != PROCON_OUTPUT_REPORT_SUBCOMMAND)
		return 0;

	uint8_t command = packet[PROCON_OUTPUT_SUBCOMMAND_OFFSET];
	fill_standard(state, timer, PROCON_INPUT_REPORT_SUBCOMMAND_REPLY, reply,
		PROCON_REPLY_SIZE);
	reply[PROCON_REPLY_SUBCOMMAND_OFFSET] = command;

	switch (command) {
	case PROCON_SUBCOMMAND_PAIRING:
		reply[PROCON_REPLY_ACK_OFFSET] = 0x81;
		reply[PROCON_REPLY_DATA_OFFSET] = 0x03;
		break;
	case PROCON_SUBCOMMAND_DEVICE_INFO:
		reply[PROCON_REPLY_ACK_OFFSET] = 0x82;
		reply[PROCON_REPLY_DATA_OFFSET] = 0x04;
		reply[PROCON_REPLY_DATA_OFFSET + 1] = 0x00;
		reply[PROCON_REPLY_DATA_OFFSET + 2] = 0x03;
		reply[PROCON_REPLY_DATA_OFFSET + 3] = 0x02;
		memcpy(reply + PROCON_REPLY_DATA_OFFSET + 4, state->mac,
			sizeof(state->mac));
		reply[PROCON_REPLY_DATA_OFFSET + 10] = 0x01;
		reply[PROCON_REPLY_DATA_OFFSET + 11] = 0x01;
		break;
	case PROCON_SUBCOMMAND_SET_INPUT_MODE:
		if (packet_size <= PROCON_OUTPUT_DATA_OFFSET)
			return 0;
		state->input_mode = packet[PROCON_OUTPUT_DATA_OFFSET];
		reply[PROCON_REPLY_ACK_OFFSET] = 0x80;
		break;
	case PROCON_SUBCOMMAND_TRIGGER_BUTTONS:
		reply[PROCON_REPLY_ACK_OFFSET] = 0x83;
		reply[PROCON_REPLY_DATA_OFFSET] =
			reply[PROCON_REPLY_DATA_OFFSET + 2] = 0x2c;
		reply[PROCON_REPLY_DATA_OFFSET + 1] =
			reply[PROCON_REPLY_DATA_OFFSET + 3] = 0x01;
		break;
	case PROCON_SUBCOMMAND_SET_SHIPMENT_MODE:
		reply[PROCON_REPLY_ACK_OFFSET] = 0x80;
		break;
	case PROCON_SUBCOMMAND_SPI_READ: {
		if (packet_size <= PROCON_SPI_REQUEST_SIZE_OFFSET)
			return 0;
		uint32_t address = (uint32_t)packet[PROCON_OUTPUT_DATA_OFFSET] |
			((uint32_t)packet[PROCON_OUTPUT_DATA_OFFSET + 1] << 8) |
			((uint32_t)packet[PROCON_OUTPUT_DATA_OFFSET + 2] << 16) |
			((uint32_t)packet[PROCON_OUTPUT_DATA_OFFSET + 3] << 24);
		uint8_t size = packet[PROCON_SPI_REQUEST_SIZE_OFFSET];
		if (size > PROCON_SPI_MAX_READ) {
			reply[PROCON_REPLY_ACK_OFFSET] = 0x83;
			break;
		}
		reply[PROCON_REPLY_ACK_OFFSET] = 0x90;
		memcpy(reply + PROCON_REPLY_DATA_OFFSET,
			packet + PROCON_OUTPUT_DATA_OFFSET, PROCON_SPI_ADDRESS_SIZE);
		reply[PROCON_SPI_REPLY_SIZE_OFFSET] = size;
		spi_read(reply + PROCON_SPI_REPLY_DATA_OFFSET, address, size);
		break;
	}
	case PROCON_SUBCOMMAND_SET_NFC_IR_CONFIG:
		reply[PROCON_REPLY_ACK_OFFSET] = 0xa0;
		memcpy(reply + PROCON_REPLY_DATA_OFFSET,
			"\x01\x00\xff\x00\x08\x00\x1b\x01", 8);
		reply[49] = 0xc8;
		break;
	case PROCON_SUBCOMMAND_SET_NFC_IR_STATE:
	case PROCON_SUBCOMMAND_SET_HOME_LIGHT:
	case PROCON_SUBCOMMAND_SET_IMU_SENSITIVITY:
		reply[PROCON_REPLY_ACK_OFFSET] = 0x80;
		break;
	case PROCON_SUBCOMMAND_SET_PLAYER_LIGHTS:
		if (packet_size <= PROCON_OUTPUT_DATA_OFFSET)
			return 0;
		state->player_lights = packet[PROCON_OUTPUT_DATA_OFFSET];
		reply[PROCON_REPLY_ACK_OFFSET] = 0x80;
		break;
	case PROCON_SUBCOMMAND_ENABLE_IMU:
		if (packet_size <= PROCON_OUTPUT_DATA_OFFSET)
			return 0;
		/* Preserve the Pro Controller handshake while motion streaming is
		 * intentionally disabled for the foreground Wi-Fi isolation test. */
		reply[PROCON_REPLY_ACK_OFFSET] = 0x80;
		break;
	case PROCON_SUBCOMMAND_ENABLE_VIBRATION:
		if (packet_size <= PROCON_OUTPUT_DATA_OFFSET)
			return 0;
		state->vibration_enabled = packet[PROCON_OUTPUT_DATA_OFFSET] == 1;
		reply[PROCON_REPLY_ACK_OFFSET] = 0x80;
		break;
	default:
		return 0;
	}

	return PROCON_REPLY_SIZE;
}
