#include <psp2kern/io/fcntl.h>
#include <psp2common/ctrl.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/proc_event.h>
#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/power.h>
#include <stdint.h>
#include <taihen.h>

#include "bond_record.h"
#include "bond_state.h"
#include "diagnostic_trace.h"
#include "motion_sample.h"
#include "procon.h"
#include "raw_l2cap.h"
#include "scebt_fw360.h"
#include "touch_map.h"

#define TRACE_PATH "ux0:data/scebt-trace.txt"
#define COMMAND_PATH "ux0:data/scebt-command.txt"
#define PEER_PATH "ux0:data/vita-ns-peer.bin"
#define PEER_RECORD_SIZE VITA_NS_PEER_RECORD_SIZE
#define LINK_KEY_PATH "ur0:tai/vita-ns-link-key.bin"
#define LINK_KEY_RECORD_SIZE VITA_NS_LINK_KEY_RECORD_SIZE
#define CONTROLLER_TITLE_ID "VITANSPAD"
#define CONTROLLER_HEARTBEAT_TIMEOUT_US 250000U
#define PAIRING_DISCOVERABLE_TIMEOUT_US 60000000U
#define RECONNECT_STALE_TIMEOUT_US 2000000U
#define RECONNECT_ACL_TIMEOUT_US 8000000U
#define RECONNECT_L2CAP_TIMEOUT_US 5000000U
#define RECONNECT_AUTH_START_DELAY_US 15000U
#define RECONNECT_HID_START_DELAY_US 15000U
#define RECONNECT_PASSIVE_WAIT_US 2000000U
#define RECONNECT_RETRY_DELAY_US 750000U
#define PEER_SAVE_RETRY_US 2000000U
#define INPUT_KEEPALIVE_US 1000000U
#define HID_BOOTSTRAP_REPORT_SIZE 51U
#define HID_BOOTSTRAP_REPORT_COUNT 10U
#define HID_INITIALIZED_REPORT_INTERVAL_US 66667U
#define RAW_INPUT_REPORT_INTERVAL_US 15000U
#define SCAN_COMMAND_TIMEOUT_US 1000000U
#define BATTERY_POLL_INTERVAL_US 2000000U
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

enum {
	HCI_EVENT_CONNECTION_COMPLETE = 0x03,
	HCI_EVENT_DISCONNECTION_COMPLETE = 0x05,
	HCI_EVENT_AUTHENTICATION_COMPLETE = 0x06,
	HCI_EVENT_ENCRYPTION_CHANGE = 0x08,
	HCI_EVENT_COMMAND_COMPLETE = 0x0e,
	HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS = 0x13,
	HCI_EVENT_LINK_KEY_REQUEST = 0x17,
	HCI_EVENT_LINK_KEY_NOTIFICATION = 0x18,
	HCI_EVENT_USER_CONFIRMATION_REQUEST = 0x33,
	HCI_OPCODE_CREATE_CONNECTION = 0x0405,
	HCI_OPCODE_DISCONNECT = 0x0406,
	HCI_OPCODE_LINK_KEY_REPLY = 0x040b,
	HCI_OPCODE_LINK_KEY_NEGATIVE_REPLY = 0x040c,
	HCI_OPCODE_AUTHENTICATION_REQUESTED = 0x0411,
	HCI_OPCODE_READ_SCAN_ENABLE = 0x0c19,
	HCI_OPCODE_WRITE_SCAN_ENABLE = 0x0c1a,
	HCI_OPCODE_READ_BD_ADDR = 0x1009,
	HCI_REASON_REMOTE_USER_TERMINATED_CONNECTION = 0x13,
};

enum {
	RAW_CONTROL_CONNECTION_IDENTIFIER_BASE = 0x50,
	RAW_CONTROL_CONFIGURATION_IDENTIFIER_BASE = 0x60,
	RAW_INTERRUPT_CONNECTION_IDENTIFIER_BASE = 0x70,
	RAW_INTERRUPT_CONFIGURATION_IDENTIFIER_BASE = 0x80,
	RAW_TRACE_TRANSPORT_COMPLETE = 0xd2,
	RAW_TRACE_VALID_INPUT = 0xea,
	RAW_TRACE_VALID_INPUT_STATE = 0xeb,
	RAW_TRACE_ESTABLISHED = 0xec,
	RAW_TRACE_INPUT_REPORT = 0xed,
	RAW_TRACE_INPUT_GATED = 0xee,
	RAW_INIT_VALID_TRANSITION_STAGE = 6,
};

enum {
	HID_CONFIGURED_CONTROL = 1U << 0,
	HID_CONFIGURED_INTERRUPT = 1U << 1,
	HID_CONFIGURED_ALL = HID_CONFIGURED_CONTROL | HID_CONFIGURED_INTERRUPT,
};

enum {
	ACTIVE_RECONNECT_IDLE = 0,
	ACTIVE_RECONNECT_PASSIVE_WAIT,
	ACTIVE_RECONNECT_PASSIVE_ACL_WAIT,
	ACTIVE_RECONNECT_ACL_PENDING,
	ACTIVE_RECONNECT_CONTROL_START,
	ACTIVE_RECONNECT_CONTROL_PENDING,
	ACTIVE_RECONNECT_INTERRUPT_START,
	ACTIVE_RECONNECT_INTERRUPT_PENDING,
	ACTIVE_RECONNECT_DISCONNECTING,
	ACTIVE_RECONNECT_RETRY_WAIT,
	ACTIVE_RECONNECT_AUTH_START,
	ACTIVE_RECONNECT_AUTH_PENDING,
	ACTIVE_RECONNECT_ENCRYPT_PENDING,
	ACTIVE_RECONNECT_CONTROL_CONFIG_START,
	ACTIVE_RECONNECT_CONTROL_CONFIG_PENDING,
	ACTIVE_RECONNECT_RAW_INTERRUPT_START,
	ACTIVE_RECONNECT_RAW_INTERRUPT_PENDING,
	ACTIVE_RECONNECT_RAW_INTERRUPT_CONFIG_START,
	ACTIVE_RECONNECT_RAW_INTERRUPT_CONFIG_PENDING,
	ACTIVE_RECONNECT_RAW_BOOTSTRAP_START,
	ACTIVE_RECONNECT_RAW_BOOTSTRAP_WAIT,
	ACTIVE_RECONNECT_RAW_DEVICE_INFO_REPLY_START,
	ACTIVE_RECONNECT_RAW_DEVICE_INFO_REPLY_WAIT,
	ACTIVE_RECONNECT_RAW_COMMAND08_REPLY_START,
	ACTIVE_RECONNECT_RAW_COMMAND08_REPLY_WAIT,
	ACTIVE_RECONNECT_RAW_COMMAND10_REPLY_START,
	ACTIVE_RECONNECT_RAW_COMMAND10_REPLY_WAIT,
	ACTIVE_RECONNECT_RAW_SPI2_REPLY_START,
	ACTIVE_RECONNECT_RAW_SPI2_REPLY_WAIT,
	ACTIVE_RECONNECT_RAW_INIT_SCRIPT_REPLY_START,
	ACTIVE_RECONNECT_RAW_INIT_SCRIPT_REPLY_WAIT,
};

typedef int (*HciCommand)(void *context, int opcode, const char *format, ...);
typedef void (*HciTransportCompletion)(void *argument);
typedef int (*HciTransportEnqueue)(int packet_type, const uint8_t *packet,
	int length, HciTransportCompletion completion, void *argument);
typedef void *(*FindConnection)(void *context, int kind,
	uint32_t handle_low, uint32_t handle_high);
typedef void *(*FindChannel)(void *connection, int kind, uint16_t key);
typedef int (*L2capConnectionResponse)(void *connection, void *channel,
	int result, int status);
typedef int (*L2capConnect)(void *connection, void *channel);
typedef int (*L2capSend)(void *connection, uint16_t cid,
	const char *format, ...);
typedef int (*ConnectRemote)(uint32_t address_low, uint32_t address_high);
typedef int (*HidChannelHandler)(void *connection, void *channel, int argument);

static tai_hook_ref_t hci_event_ref;
static tai_hook_ref_t acl_event_ref;
static tai_hook_ref_t hid_channel_ref;
static tai_hook_ref_t disconnect_direct_ref;
static tai_hook_ref_t disconnect_cleanup_ref;
static tai_hook_ref_t hid_disconnect_ref;
static tai_hook_ref_t hci_command_ref;
static tai_hook_ref_t l2cap_connect_ref;
static SceUID hci_event_uid = -1;
static SceUID acl_event_uid = -1;
static SceUID hid_channel_uid = -1;
static SceUID disconnect_direct_uid = -1;
static SceUID disconnect_cleanup_uid = -1;
static SceUID hid_disconnect_uid = -1;
static SceUID hci_command_uid = -1;
static SceUID l2cap_connect_uid = -1;
static SceUID local_name_uid = -1;
static SceUID class_movw_uid = -1;
static SceUID class_shift_uid = -1;
static SceUID io_capability_uid = -1;
static SceUID auth_requirements_uid = -1;
static SceUID sdp_db_uid = -1;
static SceUID pnp_ids_uid = -1;
static SceUID scan_mask_uid = -1;
static SceUID worker_uid = -1;
static SceUID sender_uid = -1;
static SceUID proc_event_uid = -1;
static volatile int stop_requested;
static HciCommand hci_command;
static HciTransportEnqueue hci_transport_enqueue;
static void *hci_context;
static const char *hci_name_format;
static const char *hci_class_format;
static const char *hci_eir_format;
static const char *hci_delete_key_format;
static const char *hci_address_format;
static const char *hci_link_key_reply_format;
static const char *hci_create_connection_format;
static const char *hci_local_name;
static int (*set_scan)(int);
static int (*confirm_user)(uint32_t address_low, uint32_t address_high, int accept);
static ConnectRemote connect_remote;
static HidChannelHandler hid_channel_handler;
static FindConnection find_connection;
static FindChannel find_channel;
static L2capConnectionResponse l2cap_connection_response;
static L2capConnect l2cap_connect;
static L2capSend l2cap_send;
static const char *l2cap_raw_format;
static void *pending_hid_connection;
static void *pending_hid_channel;
static volatile unsigned int l2cap_response_pending;
static uint16_t pending_l2cap_handle[2];
static uint8_t pending_l2cap_identifier[2];
static void *pending_l2cap_context[2];
static void *switch_connection;
static void *hid_control_channel;
static void *hid_interrupt_channel;
static void *deferred_hid_connection;
static void *deferred_hid_control_channel;
static volatile int active_l2cap_connect_call;
static volatile int keepalive_active;
static volatile uint16_t switch_acl_handle;
static volatile int switch_address_save_pending;
static uint32_t switch_address_save_attempted;
static volatile int switch_link_key_save_pending;
static uint32_t switch_link_key_save_attempted;
static unsigned int pending_link_key_consumed;
static volatile int input_send_ready;
static volatile unsigned int hid_configured;
static ProconState procon_state;
static uint8_t controller_mac[6];
static uint8_t procon_timer;
static volatile int subcommands_started;
static volatile int continuous_input_enabled;
static volatile unsigned int bootstrap_input_reports;
static volatile int bootstrap_output_seen;
static volatile int pairing_confirmed;
static volatile int controller_app_active;
static volatile SceUID controller_app_pid = -1;
static volatile uint32_t controller_heartbeat_time;
static volatile uint32_t controller_vita_buttons;
static volatile uint32_t controller_vita_sticks;
static volatile uint8_t controller_touch_buttons;
static volatile unsigned int controller_input_sequence;
static volatile int reconnect_requested;
static volatile int reconnect_input_latched;
static volatile int pairing_resume_requested;
static volatile int pairing_reset_requested;
static volatile int pairing_reset_state;
static volatile int force_disconnect;
static uint32_t pairing_reset_started;
static volatile int pairing_discoverable_active;
static uint32_t pairing_discoverable_started;
static volatile int active_reconnect_state;
static volatile int active_reconnect_attempts;
static uint32_t active_reconnect_started;
static void *active_reconnect_context;
static volatile int raw_acl_reconnect;
static volatile unsigned int raw_transport_completions;
static volatile uint16_t raw_control_remote_cid;
static volatile uint8_t raw_control_config_identifier;
static volatile uint8_t raw_control_request_identifier;
static volatile uint16_t raw_interrupt_remote_cid;
static volatile uint8_t raw_interrupt_connection_identifier;
static volatile uint8_t raw_interrupt_config_identifier;
static volatile uint8_t raw_interrupt_request_identifier;
static uint8_t raw_subcommand_request[PROCON_REPLY_SIZE];
static volatile size_t raw_subcommand_request_length;
static volatile uint8_t raw_init_script_stage;
static volatile int raw_input_active;
static volatile int raw_input_pending;
static unsigned int raw_connected_input_reports;

typedef struct {
	uint8_t command;
	uint8_t payload_length;
	uint8_t payload[5];
} RawInitStep;

static const RawInitStep raw_init_script[] = {
	{ PROCON_SUBCOMMAND_SET_INPUT_MODE, 1,
		{ PROCON_INPUT_REPORT_STANDARD } },
	{ PROCON_SUBCOMMAND_TRIGGER_BUTTONS, 0, { 0 } },
	{ PROCON_SUBCOMMAND_SPI_READ, 5,
		{ 0x80, 0x60, 0x00, 0x00, 0x18 } },
	{ PROCON_SUBCOMMAND_SPI_READ, 5,
		{ 0x98, 0x60, 0x00, 0x00, 0x12 } },
	{ PROCON_SUBCOMMAND_SPI_READ, 5,
		{ 0x10, 0x80, 0x00, 0x00, 0x18 } },
	{ PROCON_SUBCOMMAND_SPI_READ, 5,
		{ 0x3d, 0x60, 0x00, 0x00, 0x19 } },
	{ PROCON_SUBCOMMAND_SPI_READ, 5,
		{ 0x28, 0x80, 0x00, 0x00, 0x18 } },
	{ PROCON_SUBCOMMAND_ENABLE_IMU, 1, { 0x02 } },
	{ PROCON_SUBCOMMAND_SET_PLAYER_LIGHTS, 1, { 0x00 } },
	{ PROCON_SUBCOMMAND_ENABLE_VIBRATION, 1, { 0x01 } },
	{ PROCON_SUBCOMMAND_SET_NFC_IR_CONFIG, 1, { 0x21 } },
	{ PROCON_SUBCOMMAND_SET_PLAYER_LIGHTS, 1, { 0x01 } },
};
static volatile int active_reconnect_scan_active;
static uint32_t active_reconnect_scan_started;
static volatile int active_hid_start_call;
static void *active_hid_start_connection;
static void *active_hid_start_channel;
static volatile unsigned int scan_write_issued;
static volatile unsigned int scan_write_completed;
static volatile int scan_write_status;
static volatile unsigned int scan_read_issued;
static volatile unsigned int scan_read_completed;
static volatile int scan_read_status;
static volatile uint8_t scan_read_value;
static volatile uint32_t last_switch_activity;
static volatile int sender_stop_requested;
static volatile int sender_request_pending;
static volatile int sender_blocked;
static void *sender_connection;
static uint16_t sender_remote_cid;
static uint8_t sender_report[HID_BOOTSTRAP_REPORT_SIZE];
static unsigned int sender_report_length;
static uint32_t last_input_report_time;
static uint8_t last_sent_buttons[PROCON_BUTTON_BYTES];
static uint16_t last_sent_lx;
static uint16_t last_sent_ly;
static uint16_t last_sent_rx;
static uint16_t last_sent_ry;
static uint8_t last_sent_battery_level;
static uint32_t last_battery_poll_time;
static int last_battery_percent;
static int last_battery_charging;
static uint32_t previous_vita_buttons;
static uint32_t previous_vita_sticks;
static uint8_t previous_touch_buttons;

static void kick_active_control(void);
static void publish_active_reconnect(uint8_t phase, uint16_t psm, int result,
	uint16_t local_cid);
static void reset_raw_control_channel(void);
static int set_scan_and_wait(int discoverable);
static int force_discoverable_scan(void);

static const uint8_t procon_eir[240] = {
	0x0f, 0x09, 'P','r','o',' ','C','o','n','t','r','o','l','l','e','r',
	0x03, 0x03, 0x24, 0x11,
};

/* Compact HID SDP record followed by a valid filler record. Together they
 * exactly replace the first five (audio) records in the 3.60 local SDP DB. */
static const uint8_t hid_sdp_records[SDP_AUDIO_SIZE] = {
	0xf9,0x08,0x00,0x00,0x0a,0x00,0x01,0x00,0x11,0x08,0x00,0x01,
	0x35,0x03,0x19,0x11,0x24,0x12,0x00,0x04,0x35,0x0d,0x35,0x06,
	0x19,0x01,0x00,0x09,0x00,0x11,0x35,0x03,0x19,0x00,0x11,0x08,
	0x00,0x05,0x35,0x03,0x19,0x10,0x02,0x0e,0x00,0x06,0x35,0x09,
	0x09,0x65,0x6e,0x09,0x00,0x6a,0x09,0x01,0x00,0x0d,0x00,0x09,
	0x35,0x08,0x35,0x06,0x19,0x11,0x24,0x09,0x01,0x01,0x14,0x00,
	0x0d,0x35,0x0f,0x35,0x0d,0x35,0x06,0x19,0x01,0x00,0x09,0x00,
	0x13,0x35,0x03,0x19,0x00,0x11,0x06,0x02,0x01,0x09,0x01,0x11,
	0x05,0x02,0x02,0x08,0x08,0x05,0x02,0x03,0x08,0x21,0x05,0x02,
	0x04,0x28,0x01,0x05,0x02,0x05,0x28,0x01,0x5e,0x02,0x06,0x35,
	0x59,0x35,0x57,0x08,0x22,0x25,0x53,0x06,0x01,0xff,0x09,0x01,
	0xa1,0x01,0x75,0x08,0x85,0x21,0x09,0x21,0x95,0x30,0x81,0x02,
	0x85,0x30,0x09,0x30,0x81,0x02,0x85,0x31,0x09,0x31,0x96,0x69,
	0x01,0x81,0x02,0x85,0x32,0x09,0x32,0x81,0x02,0x85,0x33,0x09,
	0x33,0x81,0x02,0x85,0x3f,0x09,0x3f,0x95,0x0b,0x81,0x02,0x85,
	0x01,0x09,0x01,0x95,0x30,0x91,0x02,0x85,0x10,0x09,0x10,0x95,
	0x09,0x91,0x02,0x85,0x11,0x09,0x11,0x95,0x30,0x91,0x02,0x85,
	0x12,0x09,0x12,0x91,0x02,0xc0,0x0d,0x02,0x07,0x35,0x08,0x35,
	0x06,0x09,0x04,0x09,0x09,0x01,0x00,0x05,0x02,0x09,0x28,0x01,
	0x05,0x02,0x0a,0x28,0x01,0x06,0x02,0x0c,0x09,0x0c,0x80,0x05,
	0x02,0x0d,0x28,0x00,0x05,0x02,0x0e,0x28,0x00,0x29,0x08,0x00,
	0x00,0x0a,0x00,0x01,0x00,0x12,0x08,0x00,0x01,0x35,0x03,0x19,
	0xff,0xff,0x18,0x01,0x00,0x25,0x13,0x53,0x44,0x50,0x20,0x70,
	0x61,0x64,0x64,0x69,0x6e,0x67,0x20,0x72,0x65,0x63,0x6f,0x72,
	0x64,0x2e,
};

_Static_assert(sizeof(hid_sdp_records) == SDP_AUDIO_SIZE, "SDP patch size");

static int valid_sdp_records(const uint8_t *data, unsigned int size)
{
	unsigned int record = 0;
	while (record < size) {
		unsigned int record_size = data[record];
		unsigned int attribute = record + 1;
		if (record_size < 4 || record_size > size - record)
			return 0;
		while (attribute < record + record_size) {
			unsigned int attribute_size = data[attribute];
			if (attribute_size < 4 || attribute_size > record + record_size - attribute)
				return 0;
			attribute += attribute_size;
		}
		if (attribute != record + record_size)
			return 0;
		record += record_size;
	}
	return record == size;
}

int _start(SceSize argc, const void *args) __attribute__((weak, alias("module_start")));

static void publish(uint8_t type, const uint8_t *data, int length)
{
	diagnostic_trace_publish(type, data, length);
}

static int is_controller_app(SceUID pid)
{
	char title_id[16];
	memset(title_id, 0, sizeof(title_id));
	return pid >= 0 && ksceKernelGetProcessTitleId(pid, title_id,
		sizeof(title_id)) >= 0 &&
		!strncmp(title_id, CONTROLLER_TITLE_ID, sizeof(CONTROLLER_TITLE_ID) - 1);
}

static void set_controller_app_state(SceUID pid, int active, uint8_t reason)
{
	int old_active = controller_app_active;
	if (active) {
		controller_app_pid = pid;
		controller_app_active = 1;
	} else if (pid < 0 || pid == controller_app_pid) {
		controller_app_active = 0;
		reconnect_input_latched = 0;
		previous_vita_buttons = 0;
	}
	if (old_active == controller_app_active)
		return;
	uint8_t probe[8] = { 0x41, (uint8_t)controller_app_active, reason, 0,
		(uint8_t)pid, (uint8_t)(pid >> 8),
		(uint8_t)(pid >> 16), (uint8_t)(pid >> 24) };
	publish(10, probe, sizeof(probe));
}

int vitaNsPadSubmitInput(uint32_t buttons, uint32_t sticks,
	uint32_t touch_buttons)
{
	SceUID pid = ksceKernelGetProcessId();
	if (!is_controller_app(pid))
		return -1;
	uint8_t new_touch_buttons = (uint8_t)touch_buttons &
		VITA_NS_TOUCH_INPUT_MASK;
	__sync_fetch_and_add(&controller_input_sequence, 1);
	controller_vita_buttons = buttons;
	controller_vita_sticks = sticks;
	controller_touch_buttons = new_touch_buttons;
	__sync_synchronize();
	__sync_fetch_and_add(&controller_input_sequence, 1);
	controller_heartbeat_time = ksceKernelGetSystemTimeLow();
	__sync_synchronize();
	set_controller_app_state(pid, 1, 7);
	int reconnect_input = buttons != 0 ||
		(new_touch_buttons & VITA_NS_TOUCH_BUTTON_MASK) != 0;
	/* NEW PAIR remains passive/discoverable.  Any real controller button wakes
	 * a disconnected bonded controller, which pages its saved host and then
	 * actively opens HID control and interrupt.  L+R is only the controller
	 * confirmation gesture inside Change Grip/Order.  Ignore stick movement and
	 * persistent touch-layout options, and accept a stale ACL so the worker can
	 * tear it down before paging. */
	int pairing_flow_active = pairing_resume_requested ||
		pairing_reset_requested || pairing_reset_state ||
		pairing_discoverable_active;
	uint32_t activity_age = ksceKernelGetSystemTimeLow() - last_switch_activity;
	int stale_connection = switch_acl_handle &&
		activity_age >= RECONNECT_STALE_TIMEOUT_US;
	if (reconnect_input && !reconnect_input_latched && !pairing_flow_active &&
	    (!switch_acl_handle || stale_connection)) {
		active_reconnect_attempts = 0;
		reconnect_requested = 1;
	}
	reconnect_input_latched = reconnect_input;
	return 0;
}

int vitaNsPadSubmitMotion(
	const VitaNsMotionSample samples[VITA_NS_MOTION_SAMPLE_COUNT],
	uint32_t sample_count)
{
	SceUID pid = ksceKernelGetProcessId();
	if (!is_controller_app(pid))
		return -1;
	/* Keep the old syscall as an ABI-compatible no-op so an already-installed
	 * app cannot fail to launch while the no-motion VPK is being installed. */
	(void)samples;
	(void)sample_count;
	return 0;
}

int vitaNsPadDisable(void)
{
	SceUID pid = ksceKernelGetProcessId();
	if (pid != controller_app_pid)
		return -1;
	set_controller_app_state(pid, 0, 8);
	return 0;
}

int vitaNsPadStartPairing(void)
{
	SceUID pid = ksceKernelGetProcessId();
	if (!is_controller_app(pid) || pid != controller_app_pid)
		return -1;
	pairing_reset_requested = 1;
	return 0;
}

static void expire_controller_heartbeat(void)
{
	if (!controller_app_active)
		return;
	uint32_t age = ksceKernelGetSystemTimeLow() - controller_heartbeat_time;
	if (age > CONTROLLER_HEARTBEAT_TIMEOUT_US)
		set_controller_app_state(controller_app_pid, 0, 9);
}

static int controller_proc_create(SceUID pid, SceProcEventInvokeParam2 *param, int arg)
{
	(void)pid;
	(void)param;
	(void)arg;
	return 0;
}

static int controller_proc_exit(SceUID pid, SceProcEventInvokeParam1 *param, int arg)
{
	(void)param;
	(void)arg;
	if (pid == controller_app_pid)
		set_controller_app_state(pid, 0, 2);
	return 0;
}

static int controller_proc_kill(SceUID pid, SceProcEventInvokeParam1 *param, int arg)
{
	(void)param;
	(void)arg;
	if (pid == controller_app_pid)
		set_controller_app_state(pid, 0, 3);
	return 0;
}

static int controller_proc_stop(SceUID pid, int event_type,
	SceProcEventInvokeParam1 *param, int arg)
{
	(void)event_type;
	(void)param;
	(void)arg;
	if (pid == controller_app_pid)
		set_controller_app_state(pid, 0, 4);
	return 0;
}

static int controller_proc_start(SceUID pid, int event_type,
	SceProcEventInvokeParam1 *param, int arg)
{
	(void)pid;
	(void)event_type;
	(void)param;
	(void)arg;
	return 0;
}

static int controller_proc_switch(int event_id, int event_type,
	SceProcEventInvokeParam2 *param, int arg)
{
	(void)event_id;
	(void)event_type;
	(void)arg;
	if (!param)
		return 0;
	if (controller_app_active && param->pid != controller_app_pid)
		set_controller_app_state(controller_app_pid, 0, 6);
	return 0;
}

static const SceProcEventHandler controller_proc_handler = {
	.size = sizeof(SceProcEventHandler),
	.create = controller_proc_create,
	.exit = controller_proc_exit,
	.kill = controller_proc_kill,
	.stop = controller_proc_stop,
	.start = controller_proc_start,
	.switch_process = controller_proc_switch,
};

static int hci_event_hook(void *context, const uint8_t *data, int length)
{
	int scan_write_complete = data && length >= 6 &&
		data[0] == HCI_EVENT_COMMAND_COMPLETE &&
		read_le16(data + 3) == HCI_OPCODE_WRITE_SCAN_ENABLE;
	int scan_read_complete = data && length >= 7 &&
		data[0] == HCI_EVENT_COMMAND_COMPLETE &&
		read_le16(data + 3) == HCI_OPCODE_READ_SCAN_ENABLE;
	int connection_complete_event = data && length >= 11 &&
		data[0] == HCI_EVENT_CONNECTION_COMPLETE;
	int authentication_complete_event = data && length >= 5 &&
		data[0] == HCI_EVENT_AUTHENTICATION_COMPLETE;
	int encryption_change_event = data && length >= 6 &&
		data[0] == HCI_EVENT_ENCRYPTION_CHANGE;
	int link_key_request_event = data && length >= 8 &&
		data[0] == HCI_EVENT_LINK_KEY_REQUEST;
	int link_key_notification_event = data && length >= 25 &&
		data[0] == HCI_EVENT_LINK_KEY_NOTIFICATION;
	int read_bd_addr_complete = data && length >= 12 &&
		data[0] == HCI_EVENT_COMMAND_COMPLETE &&
		read_le16(data + 3) == HCI_OPCODE_READ_BD_ADDR;
	uint16_t security_event_handle =
		(authentication_complete_event || encryption_change_event) ?
		(data[3] | ((uint16_t)(data[4] & 0x0f) << 8)) : 0;
	uint32_t event_address_low = connection_complete_event ?
		data[5] | ((uint32_t)data[6] << 8) |
		((uint32_t)data[7] << 16) | ((uint32_t)data[8] << 24) : 0;
	uint16_t event_address_high = connection_complete_event ?
		data[9] | ((uint16_t)data[10] << 8) : 0;
	uint32_t request_address_low = link_key_request_event ?
		data[2] | ((uint32_t)data[3] << 8) |
		((uint32_t)data[4] << 16) | ((uint32_t)data[5] << 24) : 0;
	uint16_t request_address_high = link_key_request_event ?
		data[6] | ((uint16_t)data[7] << 8) : 0;
	int raw_link_key_request = link_key_request_event && raw_acl_reconnect &&
		bond_state_peer_matches(request_address_low, request_address_high);
	int known_peer = connection_complete_event &&
		bond_state_peer_matches(event_address_low, event_address_high);
	int explicit_peer_window = controller_app_active &&
		(pairing_discoverable_active ||
		 active_reconnect_state == ACTIVE_RECONNECT_PASSIVE_WAIT);
	int switch_connection_complete = connection_complete_event && data[2] == 0 &&
		vita_ns_valid_peer_address(event_address_low, event_address_high) &&
		(known_peer || explicit_peer_window);
	int learned_peer = switch_connection_complete && !known_peer;
	uint32_t link_key_address_low = link_key_notification_event ?
		data[2] | ((uint32_t)data[3] << 8) |
		((uint32_t)data[4] << 16) | ((uint32_t)data[5] << 24) : 0;
	uint16_t link_key_address_high = link_key_notification_event ?
		data[6] | ((uint16_t)data[7] << 8) : 0;
	int learned_link_key = link_key_notification_event &&
		bond_state_peer_matches(link_key_address_low, link_key_address_high);
	int should_confirm = confirm_user && data && length >= 8 &&
		data[0] == HCI_EVENT_USER_CONFIRMATION_REQUEST;
	uint32_t confirmation_address_low = 0;
	uint32_t confirmation_address_high = 0;
	if (should_confirm) {
		confirmation_address_low = data[2] | ((uint32_t)data[3] << 8) |
			((uint32_t)data[4] << 16) | ((uint32_t)data[5] << 24);
		confirmation_address_high = data[6] | ((uint32_t)data[7] << 8);
	}
	/* SceBt keeps the notification key in its live connection object, but the
	 * next Link Key Request proves that its persistent lookup misses it.  Copy
	 * only the current Switch peer here; disk I/O is deferred to the worker. */
	if (learned_link_key)
		bond_state_queue_link_key(link_key_address_low, link_key_address_high,
			data + 8, data[24]);
	if (read_bd_addr_complete) {
		uint8_t mac[6] = { 0, 0, 0, 0, 0, 0 };
		int valid = data[5] == 0;
		if (valid) {
			/* HCI transports BD_ADDR least-significant byte first, while
			 * Switch subcommand 0x02 expects the printed/MSB-first order used
			 * by NXBT and joycontrol. */
			for (unsigned int i = 0; i < sizeof(mac); i++)
				mac[i] = data[11 - i];
			memcpy(controller_mac, mac, sizeof(controller_mac));
			memcpy(procon_state.mac, mac, sizeof(procon_state.mac));
			__sync_synchronize();
		}
		uint8_t probe[8] = { 0x5D, (uint8_t)valid,
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5] };
		publish(10, probe, sizeof(probe));
	}
	if (switch_connection_complete) {
		if (learned_peer)
			bond_state_set_peer(event_address_low, event_address_high);
		switch_acl_handle = data[3] | ((uint16_t)(data[4] & 0x0f) << 8);
		active_reconnect_context = context;
		last_switch_activity = ksceKernelGetSystemTimeLow();
		input_send_ready = 1;
		l2cap_response_pending = 0;
		pending_hid_connection = NULL;
		pending_hid_channel = NULL;
		switch_connection = NULL;
		hid_control_channel = NULL;
		hid_interrupt_channel = NULL;
		deferred_hid_connection = NULL;
		deferred_hid_control_channel = NULL;
		hid_configured = 0;
		subcommands_started = 0;
		continuous_input_enabled = 0;
		bootstrap_input_reports = 0;
		bootstrap_output_seen = 0;
		pairing_confirmed = 0;
		keepalive_active = 1;
		last_input_report_time = 0;
		if (active_reconnect_state == ACTIVE_RECONNECT_PASSIVE_WAIT) {
			/* The Switch won the passive scan window.  Let it own the HID
			 * L2CAP setup exactly as it does during first pairing. */
			active_reconnect_state = ACTIVE_RECONNECT_PASSIVE_ACL_WAIT;
			active_reconnect_started = ksceKernelGetSystemTimeLow();
		}
	}
	if (connection_complete_event && data[2] != 0 && known_peer &&
	    active_reconnect_state == ACTIVE_RECONNECT_ACL_PENDING) {
		raw_acl_reconnect = 0;
		reset_raw_control_channel();
		active_reconnect_state = ACTIVE_RECONNECT_RETRY_WAIT;
		active_reconnect_started = ksceKernelGetSystemTimeLow();
	}
	/* Let normal SceBt cleanup run after a real controller disconnect. */
	if (data && length >= 6 &&
	    data[0] == HCI_EVENT_DISCONNECTION_COMPLETE) {
		uint16_t disconnected_handle = data[3] |
			((uint16_t)(data[4] & 0x0f) << 8);
		if (disconnected_handle == switch_acl_handle) {
			int arm_input_reconnect = controller_app_active &&
				!pairing_resume_requested && !pairing_reset_requested &&
				!pairing_reset_state && !pairing_discoverable_active;
			keepalive_active = 0;
			switch_acl_handle = 0;
			input_send_ready = 0;
			l2cap_response_pending = 0;
			pending_hid_connection = NULL;
			pending_hid_channel = NULL;
			switch_connection = NULL;
			hid_control_channel = NULL;
			hid_interrupt_channel = NULL;
			deferred_hid_connection = NULL;
			deferred_hid_control_channel = NULL;
			hid_configured = 0;
			subcommands_started = 0;
			continuous_input_enabled = 0;
			bootstrap_input_reports = 0;
			bootstrap_output_seen = 0;
			pairing_confirmed = 0;
			last_input_report_time = 0;
			active_reconnect_context = NULL;
			raw_acl_reconnect = 0;
			reset_raw_control_channel();
			if (active_reconnect_state != ACTIVE_RECONNECT_IDLE) {
				active_reconnect_state = ACTIVE_RECONNECT_RETRY_WAIT;
				active_reconnect_started = ksceKernelGetSystemTimeLow();
			} else if (arm_input_reconnect) {
				/* A real Pro Controller remains quiet after the console closes
				 * the ACL.  A later controller-button edge starts paging; do not
				 * spend reconnect attempts while the Switch is still asleep. */
				active_reconnect_attempts = 0;
				reconnect_requested = 0;
				reconnect_input_latched = 0;
				publish_active_reconnect(RAW_TRACE_INPUT_GATED, 0, 0, 0);
			}
		}
	}
	/* Keep at most one periodic input report outstanding.  SceBt's L2CAP
	 * sender can block once its ACL queue fills (notably when the Switch goes
	 * to sleep), which would otherwise stall pairing and trace processing. */
	if (data && length >= 7 &&
	    data[0] == HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS) {
		unsigned int handles = data[2];
		unsigned int offset = 3;
		while (handles-- && offset + 3 < (unsigned int)length) {
			uint16_t completed_handle = data[offset] |
				((uint16_t)(data[offset + 1] & 0x0f) << 8);
			uint16_t completed_packets = data[offset + 2] |
				((uint16_t)data[offset + 3] << 8);
			if (completed_handle == switch_acl_handle && completed_packets) {
				input_send_ready = 1;
				last_switch_activity = ksceKernelGetSystemTimeLow();
			}
			offset += 4;
		}
	}
	/* Inquiry results are high-volume and contain no traffic addressed to us.
	 * Never copy the 16-byte Bluetooth key into the on-disk trace. */
	if (link_key_notification_event) {
		uint8_t redacted_key_event[9] = { 0x18, 7,
			data[2], data[3], data[4], data[5], data[6], data[7], data[24] };
		publish(4, redacted_key_event, sizeof(redacted_key_event));
	} else if (!data || length < 1 ||
	    (data[0] != 0x2F && data[0] != 0x22 && data[0] != 0x07)) {
		/* Once raw input forwarding starts, Number Of Completed Packets is
		 * only flow control.  Logging every completion would turn ordinary
		 * controller traffic into sustained kernel file I/O. */
		if (!data || data[0] != HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS ||
		    !raw_input_active)
			publish(4, data, length);
	}
	int result = TAI_CONTINUE(int, hci_event_ref, context, data, length);
	/* BlueZ answers this event from its persistent bond database before an
	 * outbound L2CAP connect proceeds.  SceBt does not perform that lookup for
	 * the raw controller ACL created by connect_switch_peer(), so answer it
	 * after its event handler has returned.  Keep the native/passive path
	 * untouched: it already owns its authentication policy. */
	if (raw_link_key_request && hci_command && hci_link_key_reply_format) {
		uint8_t saved_key[16];
		uint8_t saved_type = 0;
		int reply_result = -1;
		int found = bond_state_read_link_key_for_peer(request_address_low,
			request_address_high, saved_key, &saved_type);
		if (found) {
			uint64_t address = request_address_low |
				((uint64_t)request_address_high << 32);
			reply_result = hci_command(hci_context,
				HCI_OPCODE_LINK_KEY_REPLY,
				hci_link_key_reply_format, address, saved_key,
				sizeof(saved_key));
		}
		uint8_t key_probe[10] = { 0x5A, 6,
			found ? saved_type : 0xff,
			(uint8_t)reply_result, (uint8_t)(reply_result >> 8),
			(uint8_t)(reply_result >> 16),
			(uint8_t)(reply_result >> 24),
			(uint8_t)request_address_low,
			(uint8_t)request_address_high,
			(uint8_t)(request_address_high >> 8) };
		publish(10, key_probe, sizeof(key_probe));
		memset(saved_key, 0, sizeof(saved_key));
	}
	if (learned_peer)
		switch_address_save_pending = 1;
	/* Publish completion only after SceBt has consumed it.  This prevents the
	 * worker from starting the next stateful scan transition while SceBt still
	 * has the preceding command marked as pending. */
	if (scan_write_complete) {
		scan_write_status = data[5];
		__sync_synchronize();
		__sync_fetch_and_add(&scan_write_completed, 1);
	}
	if (scan_read_complete) {
		scan_read_status = data[5];
		scan_read_value = data[6];
		__sync_synchronize();
		__sync_fetch_and_add(&scan_read_completed, 1);
	}
	/* The outbound connection object is not safe to use until SceBt's original
	 * handler has consumed Connection Complete.  Publishing AUTH_START
	 * before TAI_CONTINUE lets the higher-priority worker race partially
	 * initialized handle/channel state, producing HCI status 0x02. */
	if (switch_connection_complete &&
	    active_reconnect_state == ACTIVE_RECONNECT_ACL_PENDING) {
		active_reconnect_state = ACTIVE_RECONNECT_AUTH_START;
		active_reconnect_started = ksceKernelGetSystemTimeLow();
	}
	if (authentication_complete_event &&
	    security_event_handle == switch_acl_handle &&
	    active_reconnect_state == ACTIVE_RECONNECT_AUTH_PENDING) {
		if (data[2] == 0) {
			active_reconnect_state = ACTIVE_RECONNECT_ENCRYPT_PENDING;
			active_reconnect_started = ksceKernelGetSystemTimeLow();
			/* Native SceBt starts encryption for connections owned by its HID
			 * host path.  A raw controller ACL has no such owner: the trace shows
			 * successful authentication followed directly by a remote 0x13
			 * disconnect.  Start encryption explicitly only for that raw path. */
			if (raw_acl_reconnect) {
				uint16_t handle = switch_acl_handle;
				int encryption_result = hci_command ?
					hci_command(hci_context, 0x0413, "21", handle, 1) : -1;
				publish_active_reconnect(0x0f, 0x0413,
					encryption_result, handle);
				if (encryption_result < 0) {
					active_reconnect_state =
						ACTIVE_RECONNECT_DISCONNECTING;
					active_reconnect_started = 0;
				}
			}
		} else {
			active_reconnect_state = ACTIVE_RECONNECT_DISCONNECTING;
			active_reconnect_started = 0;
		}
	}
	if (encryption_change_event && security_event_handle == switch_acl_handle &&
	    (active_reconnect_state == ACTIVE_RECONNECT_AUTH_START ||
	     active_reconnect_state == ACTIVE_RECONNECT_AUTH_PENDING ||
	     active_reconnect_state == ACTIVE_RECONNECT_ENCRYPT_PENDING)) {
		if (data[2] == 0 && data[5] != 0) {
			active_reconnect_state = ACTIVE_RECONNECT_CONTROL_START;
			active_reconnect_started = ksceKernelGetSystemTimeLow();
		}
		/* A failed Encryption Change can be followed by success when native
		 * SceBt already had the same command pending.  Keep waiting until the
		 * success event, a real disconnect, or the bounded timeout. */
	}
	if (should_confirm)
		confirm_user(confirmation_address_low, confirmation_address_high, 1);
	return result;
}

static int is_switch_connection(const void *connection)
{
	uint32_t address_low;
	uint16_t address_high;
	const uint32_t *words = (const uint32_t *)connection;
	return connection && bond_state_read_peer(&address_low, &address_high) &&
		words[SCEBT_CONNECTION_ADDRESS_LOW_WORD] == address_low &&
		(words[SCEBT_CONNECTION_ADDRESS_HIGH_WORD] & 0xffff) == address_high;
}

static int reconnect_security_pending(void)
{
	return active_reconnect_state == ACTIVE_RECONNECT_ACL_PENDING ||
		active_reconnect_state == ACTIVE_RECONNECT_AUTH_START ||
		active_reconnect_state == ACTIVE_RECONNECT_AUTH_PENDING ||
		active_reconnect_state == ACTIVE_RECONNECT_ENCRYPT_PENDING;
}

static int l2cap_connect_hook(void *connection, void *channel)
{
	if (active_l2cap_connect_call)
		return TAI_CONTINUE(int, l2cap_connect_ref, connection, channel);
	uint16_t psm = channel ? *(const uint16_t *)
		((const uint8_t *)channel + SCEBT_CHANNEL_PSM_OFFSET) : 0;
	if (psm == L2CAP_PSM_HID_CONTROL && reconnect_security_pending() &&
	    is_switch_connection(connection)) {
		uint16_t local_cid = *(const uint16_t *)
			((const uint8_t *)channel + SCEBT_CHANNEL_LOCAL_CID_OFFSET);
		uint32_t channel_flags = *(const uint32_t *)
			((const uint8_t *)channel + SCEBT_CHANNEL_FLAGS_OFFSET);
		deferred_hid_connection = connection;
		deferred_hid_control_channel = channel;
		/* connect_remote starts HID while authentication is still pending on
		 * 3.60.  Returning success preserves its initialized channel; the
		 * worker submits this exact PSM 0x11 only after Encryption Change. */
		publish_active_reconnect(0xe7, psm,
			(int)(channel_flags & 0xffffU), local_cid);
		return 0;
	}
	return TAI_CONTINUE(int, l2cap_connect_ref, connection, channel);
}

static int should_keep_switch(void *connection)
{
	return keepalive_active && connection == pending_hid_connection &&
		is_switch_connection(connection);
}

static void publish_disconnect_suppressed(uint8_t path)
{
	uint8_t probe[4] = { 0x44, path, 0, 0 };
	publish(8, probe, sizeof(probe));
}

static int disconnect_direct_hook(void *connection)
{
	if (should_keep_switch(connection)) {
		publish_disconnect_suppressed(1);
		return 0;
	}
	return TAI_CONTINUE(int, disconnect_direct_ref, connection);
}

static int disconnect_cleanup_hook(void *context, void *connection)
{
	if (should_keep_switch(connection)) {
		publish_disconnect_suppressed(2);
		return 0;
	}
	return TAI_CONTINUE(int, disconnect_cleanup_ref, context, connection);
}

static int hid_disconnect_hook(void *connection)
{
	if (should_keep_switch(connection)) {
		publish_disconnect_suppressed(3);
		return 0;
	}
	return TAI_CONTINUE(int, hid_disconnect_ref, connection);
}

/* Preserve the raw AAPCS argument word stream for SceBt's custom formatter.
 * Extra words are ignored according to the format string. */
static int hci_command_hook(void *context, int opcode, const char *format,
	uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
	uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7,
	uint32_t a8, uint32_t a9, uint32_t a10, uint32_t a11)
{
	uint8_t original_role_switch = (uint8_t)a6;
	int role_switch_overridden = opcode == HCI_OPCODE_CREATE_CONNECTION &&
		format == hci_create_connection_format &&
		bond_state_peer_matches(a1, (uint16_t)a2);
	if (role_switch_overridden)
		a6 = 1;
	uint8_t command_probe[10] = {
		0x43, (uint8_t)opcode, (uint8_t)(opcode >> 8),
		(uint8_t)keepalive_active,
		(uint8_t)a0, (uint8_t)(a0 >> 8),
		(uint8_t)(a0 >> 16), (uint8_t)(a0 >> 24),
		(uint8_t)switch_acl_handle, (uint8_t)(switch_acl_handle >> 8),
	};
	publish(9, command_probe, sizeof(command_probe));
	/* Event 0x17 reaches SceBt's normal key lookup first.  On 3.60 a miss
	 * emits Link Key Request Negative Reply (0x040C).  Replace that command at
	 * the same call site with the positive form used by SceBt's found-key
	 * branch.  `6#` is BD_ADDR followed by a pointer/length blob. */
	if (opcode == HCI_OPCODE_LINK_KEY_NEGATIVE_REPLY &&
	    format == hci_address_format &&
	    hci_link_key_reply_format) {
		uint8_t saved_key[16];
		uint8_t saved_type = 0;
		uint32_t address_low = a1;
		uint16_t address_high = (uint16_t)a2;
		if (bond_state_read_link_key_for_peer(address_low, address_high,
		    saved_key, &saved_type)) {
			uint64_t address = address_low |
				((uint64_t)address_high << 32);
			int result = TAI_CONTINUE(int, hci_command_ref, context,
				HCI_OPCODE_LINK_KEY_REPLY, hci_link_key_reply_format,
				address, saved_key, sizeof(saved_key));
			uint8_t key_probe[10] = { 0x5A, 1, saved_type,
				(uint8_t)result, (uint8_t)(result >> 8),
				(uint8_t)(result >> 16), (uint8_t)(result >> 24),
				(uint8_t)address_low, (uint8_t)address_high,
				(uint8_t)(address_high >> 8) };
			publish(10, key_probe, sizeof(key_probe));
			memset(saved_key, 0, sizeof(saved_key));
			return result;
		}
	}
	if (opcode == HCI_OPCODE_DISCONNECT && !force_disconnect &&
	    keepalive_active &&
	    (a0 & 0x0fff) == switch_acl_handle) {
		publish_disconnect_suppressed(4);
		return 0;
	}
	int result = TAI_CONTINUE(int, hci_command_ref, context, opcode, format,
		a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
	if (role_switch_overridden) {
		uint8_t role_probe[10] = { 0x58, 1, original_role_switch,
			(uint8_t)a6, (uint8_t)result, (uint8_t)(result >> 8),
			(uint8_t)(result >> 16), (uint8_t)(result >> 24),
			(uint8_t)a1, (uint8_t)a2 };
		publish(10, role_probe, sizeof(role_probe));
	}
	if (result >= 0 && opcode == HCI_OPCODE_WRITE_SCAN_ENABLE)
		__sync_fetch_and_add(&scan_write_issued, 1);
	if (result >= 0 && opcode == HCI_OPCODE_READ_SCAN_ENABLE)
		__sync_fetch_and_add(&scan_read_issued, 1);
	return result;
}

static int hid_channel_hook(void *connection, void *channel, int argument)
{
	if (active_hid_start_call && connection == active_hid_start_connection &&
	    channel == active_hid_start_channel)
		return TAI_CONTINUE(int, hid_channel_ref, connection, channel, argument);
	if (channel && is_switch_connection(connection)) {
		uint16_t psm = *(const uint16_t *)((const uint8_t *)channel +
			SCEBT_CHANNEL_PSM_OFFSET);
		if (psm == L2CAP_PSM_HID_CONTROL ||
		    psm == L2CAP_PSM_HID_INTERRUPT) {
			uint16_t local_cid = *(const uint16_t *)
				((const uint8_t *)channel + SCEBT_CHANNEL_LOCAL_CID_OFFSET);
			uint16_t remote_cid = *(const uint16_t *)
				((const uint8_t *)channel + SCEBT_CHANNEL_REMOTE_CID_OFFSET);
			pending_hid_connection = connection;
			pending_hid_channel = channel;
			switch_connection = connection;
			if (psm == L2CAP_PSM_HID_CONTROL)
				hid_control_channel = channel;
			else
				hid_interrupt_channel = channel;
			keepalive_active = 1;
			uint8_t probe[8] = { 0x48, (uint8_t)psm,
				(uint8_t)(psm >> 8), (uint8_t)argument,
				(uint8_t)local_cid, (uint8_t)(local_cid >> 8),
				(uint8_t)remote_cid, (uint8_t)(remote_cid >> 8) };
			publish(7, probe, sizeof(probe));
			/* The stock callback initiates an HID-host connection in the
			 * opposite direction.  Keep the incoming channel allocated and let
			 * acl_event_hook complete its pending response instead. */
			return 0;
		}
	}
	return TAI_CONTINUE(int, hid_channel_ref, connection, channel, argument);
}

static void send_initial_input_after_config(uint32_t configured_at)
{
	uint16_t remote_cid = hid_interrupt_channel ? *(const uint16_t *)
		((const uint8_t *)hid_interrupt_channel +
			SCEBT_CHANNEL_REMOTE_CID_OFFSET) : 0;
	uint8_t report[PROCON_INPUT_SIZE];
	memset(report, 0, sizeof(report));
	report[0] = PROCON_HID_DATA_INPUT;
	report[1] = PROCON_INPUT_REPORT_STANDARD;

	uint32_t send_started = ksceKernelGetSystemTimeLow();
	int result = switch_connection && remote_cid && l2cap_send ?
		l2cap_send(switch_connection, remote_cid, l2cap_raw_format,
			report[0], report + 1, sizeof(report) - 1) : -1;
	uint32_t send_finished = ksceKernelGetSystemTimeLow();
	if (result >= 0) {
		bootstrap_input_reports = 1;
		last_input_report_time = send_finished;
	} else {
		/* Let the normal worker retry if the callback-side send could not be
		 * queued by SceBt. */
		input_send_ready = 1;
	}
	uint32_t queue_delay = send_started - configured_at;
	uint32_t send_duration = send_finished - send_started;
	uint8_t probe[12] = {
		0x5B, report[0], report[1], (uint8_t)result,
		(uint8_t)(result >> 8), (uint8_t)remote_cid,
		(uint8_t)(remote_cid >> 8), (uint8_t)queue_delay,
		(uint8_t)(queue_delay >> 8), (uint8_t)send_duration,
		(uint8_t)(send_duration >> 8), (uint8_t)bootstrap_input_reports,
	};
	publish(10, probe, sizeof(probe));
}

static int raw_init_script_matches(unsigned int stage, const uint8_t *data,
	uint16_t l2cap_length)
{
	if (stage >= ARRAY_SIZE(raw_init_script) ||
	    !data || l2cap_length != PROCON_REPLY_SIZE ||
	    data[RAW_ACL_HEADER_SIZE] != PROCON_HID_DATA_OUTPUT ||
	    data[RAW_ACL_HEADER_SIZE + 1] != PROCON_OUTPUT_REPORT_SUBCOMMAND)
		return 0;
	const RawInitStep *step = &raw_init_script[stage];
	const uint8_t *output = data + RAW_ACL_HEADER_SIZE;
	return output[PROCON_OUTPUT_SUBCOMMAND_OFFSET] == step->command &&
		!memcmp(output + PROCON_OUTPUT_DATA_OFFSET, step->payload,
			step->payload_length);
}

static int acl_event_hook(void *context, const uint8_t *data, int length)
{
	publish(2, data, length);
	uint16_t acl_handle_flags = data && length >= 2 ? read_le16(data) : 0;
	int l2cap_packet = data && length >= RAW_ACL_HEADER_SIZE &&
		(acl_handle_flags & HCI_ACL_PB_MASK) ==
		HCI_ACL_PB_FIRST_NON_FLUSHABLE;
	uint16_t packet_handle = l2cap_packet ?
		(acl_handle_flags & HCI_ACL_HANDLE_MASK) : 0;
	if (packet_handle && packet_handle == switch_acl_handle)
		last_switch_activity = ksceKernelGetSystemTimeLow();
	uint16_t l2cap_length = l2cap_packet ? read_le16(data + 4) : 0;
	uint16_t l2cap_cid = l2cap_packet ? read_le16(data + 6) : 0;
	uint16_t signaling_command_length = l2cap_packet &&
		l2cap_cid == L2CAP_SIGNALING_CID && length >= 12 ?
		read_le16(data + L2CAP_SIGNALING_LENGTH_OFFSET) : 0;
	int configuration_request_packet = l2cap_packet &&
		l2cap_cid == L2CAP_SIGNALING_CID && length >= 16 &&
		data[L2CAP_SIGNALING_COMMAND_OFFSET] ==
			L2CAP_SIGNAL_CONFIGURATION_REQUEST &&
		signaling_command_length >= 4 &&
		(unsigned int)length >= 12U + signaling_command_length;
	uint16_t configuration_destination_cid = configuration_request_packet ?
		read_le16(data + L2CAP_SIGNALING_DATA_OFFSET) : 0;
	int configuration_response_packet = l2cap_packet &&
		l2cap_cid == L2CAP_SIGNALING_CID && length >= 18 &&
		data[L2CAP_SIGNALING_COMMAND_OFFSET] ==
			L2CAP_SIGNAL_CONFIGURATION_RESPONSE &&
		signaling_command_length >= 6 &&
		(unsigned int)length >= 12U + signaling_command_length;
	uint16_t configuration_result = configuration_response_packet ?
		read_le16(data + L2CAP_SIGNALING_DATA_OFFSET + 4) : 0xffff;
	int configuration_response = configuration_response_packet &&
		configuration_result == L2CAP_CONFIGURATION_SUCCESS;
	uint16_t configured_cid = configuration_response_packet ?
		read_le16(data + L2CAP_SIGNALING_DATA_OFFSET) : 0;
	uint16_t control_cid = hid_control_channel ? *(const uint16_t *)
		((const uint8_t *)hid_control_channel +
			SCEBT_CHANNEL_LOCAL_CID_OFFSET) : 0;
	uint16_t control_remote_cid = hid_control_channel ? *(const uint16_t *)
		((const uint8_t *)hid_control_channel +
			SCEBT_CHANNEL_REMOTE_CID_OFFSET) : 0;
	uint16_t interrupt_cid = hid_interrupt_channel ? *(const uint16_t *)
		((const uint8_t *)hid_interrupt_channel +
			SCEBT_CHANNEL_LOCAL_CID_OFFSET) : 0;
	uint16_t interrupt_remote_cid = hid_interrupt_channel ? *(const uint16_t *)
		((const uint8_t *)hid_interrupt_channel +
			SCEBT_CHANNEL_REMOTE_CID_OFFSET) : 0;
	int control_configured = configured_cid &&
		(configured_cid == control_cid || configured_cid == control_remote_cid);
	int interrupt_configured = configured_cid &&
		(configured_cid == interrupt_cid || configured_cid == interrupt_remote_cid);
	int send_initial_after_config = 0;
	uint32_t interrupt_configured_at = 0;
	if (configuration_response && interrupt_configured &&
	    active_reconnect_state == ACTIVE_RECONNECT_INTERRUPT_PENDING) {
		hid_configured |= HID_CONFIGURED_INTERRUPT;
		active_reconnect_state = ACTIVE_RECONNECT_IDLE;
		active_reconnect_attempts = 0;
	} else if (configuration_response && control_configured) {
		hid_configured |= HID_CONFIGURED_CONTROL;
		if (active_reconnect_state == ACTIVE_RECONNECT_CONTROL_PENDING) {
			active_reconnect_state = ACTIVE_RECONNECT_INTERRUPT_START;
			active_reconnect_started = ksceKernelGetSystemTimeLow();
		}
	} else if (configuration_response && interrupt_configured) {
		hid_configured |= HID_CONFIGURED_INTERRUPT;
	} else if (configuration_response_packet && configuration_result != 0 &&
	    active_reconnect_state != ACTIVE_RECONNECT_IDLE) {
		active_reconnect_state = ACTIVE_RECONNECT_DISCONNECTING;
		active_reconnect_started = 0;
	}
	if (raw_acl_reconnect && packet_handle == switch_acl_handle &&
	    active_reconnect_state == ACTIVE_RECONNECT_CONTROL_CONFIG_PENDING &&
	    configuration_response_packet &&
	    data[L2CAP_SIGNALING_IDENTIFIER_OFFSET] ==
		raw_control_request_identifier &&
	    configured_cid == L2CAP_LOCAL_CID_HID_CONTROL &&
	    read_le16(data + L2CAP_SIGNALING_DATA_OFFSET + 2) == 0) {
		publish_active_reconnect(0xd7, L2CAP_PSM_HID_CONTROL,
			configuration_result, configured_cid);
		if (configuration_result == 0) {
			active_reconnect_attempts = 2;
			if (__sync_bool_compare_and_swap(&active_reconnect_state,
			    ACTIVE_RECONNECT_CONTROL_CONFIG_PENDING,
			    ACTIVE_RECONNECT_RAW_INTERRUPT_START))
				active_reconnect_started = ksceKernelGetSystemTimeLow();
		} else if (__sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_CONTROL_CONFIG_PENDING,
		    ACTIVE_RECONNECT_DISCONNECTING)) {
			active_reconnect_started = 0;
		}
	}
	if (raw_acl_reconnect && packet_handle == switch_acl_handle &&
	    active_reconnect_state ==
		ACTIVE_RECONNECT_RAW_INTERRUPT_CONFIG_PENDING &&
	    configuration_response_packet &&
	    data[L2CAP_SIGNALING_IDENTIFIER_OFFSET] ==
		raw_interrupt_request_identifier &&
	    configured_cid == L2CAP_LOCAL_CID_HID_INTERRUPT &&
	    read_le16(data + L2CAP_SIGNALING_DATA_OFFSET + 2) == 0) {
		publish_active_reconnect(0xdd, L2CAP_PSM_HID_INTERRUPT,
			configuration_result, configured_cid);
		if (configuration_result == 0) {
			active_reconnect_attempts = 2;
			if (__sync_bool_compare_and_swap(&active_reconnect_state,
			    ACTIVE_RECONNECT_RAW_INTERRUPT_CONFIG_PENDING,
			    ACTIVE_RECONNECT_RAW_BOOTSTRAP_START))
				active_reconnect_started = ksceKernelGetSystemTimeLow();
		} else if (__sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_RAW_INTERRUPT_CONFIG_PENDING,
		    ACTIVE_RECONNECT_DISCONNECTING)) {
			active_reconnect_started = 0;
		}
	}
	int raw_bootstrap_first_wait = active_reconnect_state ==
		ACTIVE_RECONNECT_RAW_BOOTSTRAP_WAIT;
	int raw_device_info_reply_wait = active_reconnect_state ==
		ACTIVE_RECONNECT_RAW_DEVICE_INFO_REPLY_WAIT;
	int raw_command08_reply_wait = active_reconnect_state ==
		ACTIVE_RECONNECT_RAW_COMMAND08_REPLY_WAIT;
	int raw_command10_reply_wait = active_reconnect_state ==
		ACTIVE_RECONNECT_RAW_COMMAND10_REPLY_WAIT;
	int raw_spi2_reply_wait = active_reconnect_state ==
		ACTIVE_RECONNECT_RAW_SPI2_REPLY_WAIT;
	int raw_init_script_reply_wait = active_reconnect_state ==
		ACTIVE_RECONNECT_RAW_INIT_SCRIPT_REPLY_WAIT;
	int raw_bootstrap_output = raw_acl_reconnect &&
		packet_handle == switch_acl_handle &&
		(raw_bootstrap_first_wait || raw_device_info_reply_wait ||
		 raw_command08_reply_wait || raw_command10_reply_wait ||
		 raw_spi2_reply_wait) &&
		l2cap_packet && l2cap_cid == L2CAP_LOCAL_CID_HID_INTERRUPT &&
		l2cap_length >= 2 &&
		(unsigned int)length >=
			(unsigned int)RAW_ACL_HEADER_SIZE + l2cap_length &&
		data[RAW_ACL_HEADER_SIZE] == PROCON_HID_DATA_OUTPUT;
	if (raw_bootstrap_output) {
		const uint8_t *output = data + RAW_ACL_HEADER_SIZE;
		uint8_t capture_phase = raw_bootstrap_first_wait ? 0xdf :
			(raw_device_info_reply_wait ? 0xe1 :
			 (raw_command08_reply_wait ? 0xe3 :
			  (raw_command10_reply_wait ? 0xe5 : 0xe7)));
		publish_active_reconnect(capture_phase,
			L2CAP_PSM_HID_INTERRUPT, l2cap_length, l2cap_cid);
		int device_info_request = raw_bootstrap_first_wait &&
			l2cap_length == PROCON_REPLY_SIZE &&
			output[1] == PROCON_OUTPUT_REPORT_SUBCOMMAND &&
			output[PROCON_OUTPUT_SUBCOMMAND_OFFSET] ==
				PROCON_SUBCOMMAND_DEVICE_INFO;
		int command08_request = raw_device_info_reply_wait &&
			l2cap_length == PROCON_REPLY_SIZE &&
			output[1] == PROCON_OUTPUT_REPORT_SUBCOMMAND &&
			output[PROCON_OUTPUT_SUBCOMMAND_OFFSET] ==
				PROCON_SUBCOMMAND_SET_SHIPMENT_MODE;
		int command10_request = raw_command08_reply_wait &&
			l2cap_length == PROCON_REPLY_SIZE &&
			output[1] == PROCON_OUTPUT_REPORT_SUBCOMMAND &&
			output[PROCON_OUTPUT_SUBCOMMAND_OFFSET] ==
				PROCON_SUBCOMMAND_SPI_READ &&
			!memcmp(output + PROCON_OUTPUT_DATA_OFFSET,
				"\x00\x60\x00\x00\x10", 5);
		int spi2_request = raw_command10_reply_wait &&
			l2cap_length == PROCON_REPLY_SIZE &&
			output[1] == PROCON_OUTPUT_REPORT_SUBCOMMAND &&
			output[PROCON_OUTPUT_SUBCOMMAND_OFFSET] ==
				PROCON_SUBCOMMAND_SPI_READ &&
			!memcmp(output + PROCON_OUTPUT_DATA_OFFSET,
				"\x50\x60\x00\x00\x0d", 5);
		int init_script_first_request = raw_spi2_reply_wait &&
			raw_init_script_matches(0, data, l2cap_length);
		if (device_info_request || command08_request || command10_request ||
		    spi2_request || init_script_first_request) {
			memcpy(raw_subcommand_request, output,
				PROCON_REPLY_SIZE);
			raw_subcommand_request_length = PROCON_REPLY_SIZE;
			if (init_script_first_request)
				raw_init_script_stage = 0;
			__sync_synchronize();
			int expected_state = device_info_request ?
				ACTIVE_RECONNECT_RAW_BOOTSTRAP_WAIT :
				(command08_request ?
				 ACTIVE_RECONNECT_RAW_DEVICE_INFO_REPLY_WAIT :
				 (command10_request ?
				  ACTIVE_RECONNECT_RAW_COMMAND08_REPLY_WAIT :
				  (spi2_request ?
				   ACTIVE_RECONNECT_RAW_COMMAND10_REPLY_WAIT :
				   ACTIVE_RECONNECT_RAW_SPI2_REPLY_WAIT)));
			int next_state = device_info_request ?
				ACTIVE_RECONNECT_RAW_DEVICE_INFO_REPLY_START :
				(command08_request ?
				 ACTIVE_RECONNECT_RAW_COMMAND08_REPLY_START :
				 (command10_request ?
				  ACTIVE_RECONNECT_RAW_COMMAND10_REPLY_START :
				  (spi2_request ?
				   ACTIVE_RECONNECT_RAW_SPI2_REPLY_START :
				   ACTIVE_RECONNECT_RAW_INIT_SCRIPT_REPLY_START)));
			if (__sync_bool_compare_and_swap(&active_reconnect_state,
			    expected_state, next_state))
				active_reconnect_started = ksceKernelGetSystemTimeLow();
		} else {
			int expected_state = raw_bootstrap_first_wait ?
				ACTIVE_RECONNECT_RAW_BOOTSTRAP_WAIT :
				(raw_device_info_reply_wait ?
				 ACTIVE_RECONNECT_RAW_DEVICE_INFO_REPLY_WAIT :
				 (raw_command08_reply_wait ?
				  ACTIVE_RECONNECT_RAW_COMMAND08_REPLY_WAIT :
				  (raw_command10_reply_wait ?
				   ACTIVE_RECONNECT_RAW_COMMAND10_REPLY_WAIT :
				   ACTIVE_RECONNECT_RAW_SPI2_REPLY_WAIT)));
			if (__sync_bool_compare_and_swap(&active_reconnect_state,
			    expected_state, ACTIVE_RECONNECT_DISCONNECTING))
				active_reconnect_started = 0;
		}
	}
	int raw_init_script_output = raw_acl_reconnect &&
		packet_handle == switch_acl_handle && raw_init_script_reply_wait &&
		l2cap_packet && l2cap_cid == L2CAP_LOCAL_CID_HID_INTERRUPT &&
		l2cap_length >= 2 &&
		(unsigned int)length >=
			(unsigned int)RAW_ACL_HEADER_SIZE + l2cap_length &&
		data[RAW_ACL_HEADER_SIZE] == PROCON_HID_DATA_OUTPUT;
	if (raw_init_script_output) {
		const uint8_t *output = data + RAW_ACL_HEADER_SIZE;
		unsigned int next_stage = (unsigned int)raw_init_script_stage + 1U;
		uint8_t command = l2cap_length >= PROCON_OUTPUT_DATA_OFFSET &&
			output[1] == PROCON_OUTPUT_REPORT_SUBCOMMAND ?
			output[PROCON_OUTPUT_SUBCOMMAND_OFFSET] : 0xff;
		publish_active_reconnect(0xe9,
			(uint16_t)((next_stage << 8) | command),
			l2cap_length, l2cap_cid);
		unsigned int script_count = ARRAY_SIZE(raw_init_script);
		if (next_stage < script_count &&
		    raw_init_script_matches(next_stage, data, l2cap_length)) {
			memcpy(raw_subcommand_request, output,
				PROCON_REPLY_SIZE);
			raw_subcommand_request_length = PROCON_REPLY_SIZE;
			raw_init_script_stage = (uint8_t)next_stage;
			__sync_synchronize();
			if (__sync_bool_compare_and_swap(&active_reconnect_state,
			    ACTIVE_RECONNECT_RAW_INIT_SCRIPT_REPLY_WAIT,
			    ACTIVE_RECONNECT_RAW_INIT_SCRIPT_REPLY_START))
				active_reconnect_started = ksceKernelGetSystemTimeLow();
		} else if (__sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_RAW_INIT_SCRIPT_REPLY_WAIT,
		    ACTIVE_RECONNECT_DISCONNECTING)) {
			active_reconnect_started = 0;
		}
	}
	if (configuration_response && interrupt_configured &&
	    hid_configured == HID_CONFIGURED_ALL && !subcommands_started &&
	    !bootstrap_input_reports && switch_connection &&
	    hid_interrupt_channel && l2cap_send) {
		/* NXBT writes its first 50-byte A1 30 report immediately after the
		 * interrupt socket's connect() completes.  The trace worker can be busy
		 * flushing SDP traffic long enough for Switch to tear both HID channels
		 * down first, so reserve the sender and mirror that timing after SceBt's
		 * receive callback has unwound. */
		input_send_ready = 0;
		interrupt_configured_at = ksceKernelGetSystemTimeLow();
		send_initial_after_config = 1;
	}
	int connection_response_packet = l2cap_packet &&
		l2cap_cid == L2CAP_SIGNALING_CID && length >= 20 &&
		data[L2CAP_SIGNALING_COMMAND_OFFSET] ==
			L2CAP_SIGNAL_CONNECTION_RESPONSE &&
		signaling_command_length == 8;
	uint16_t connection_destination_cid = connection_response_packet ?
			read_le16(data + L2CAP_SIGNALING_DATA_OFFSET) : 0;
	uint16_t connection_source_cid = connection_response_packet ?
			read_le16(data + L2CAP_SIGNALING_DATA_OFFSET + 2) : 0;
	uint16_t connection_result = connection_response_packet ?
			read_le16(data + L2CAP_SIGNALING_DATA_OFFSET + 4) : 0xffff;
	uint8_t expected_raw_identifier = (uint8_t)(
		RAW_CONTROL_CONNECTION_IDENTIFIER_BASE +
		((unsigned int)active_reconnect_attempts & 3U));
	if (raw_acl_reconnect && packet_handle == switch_acl_handle &&
	    active_reconnect_state == ACTIVE_RECONNECT_CONTROL_PENDING &&
	    connection_response_packet &&
	    data[L2CAP_SIGNALING_IDENTIFIER_OFFSET] == expected_raw_identifier &&
	    connection_source_cid == L2CAP_LOCAL_CID_HID_CONTROL) {
		publish_active_reconnect(0xd3, L2CAP_PSM_HID_CONTROL,
			connection_result,
			connection_destination_cid);
		if (connection_result == 0 &&
		    connection_destination_cid >= L2CAP_FIRST_DYNAMIC_CID) {
			raw_control_remote_cid = connection_destination_cid;
			__sync_synchronize();
		} else if (connection_result > 1) {
			active_reconnect_state = ACTIVE_RECONNECT_DISCONNECTING;
			active_reconnect_started = 0;
		}
	}
	if (raw_acl_reconnect && packet_handle == switch_acl_handle &&
	    active_reconnect_state == ACTIVE_RECONNECT_RAW_INTERRUPT_PENDING &&
	    connection_response_packet && raw_interrupt_connection_identifier &&
	    data[L2CAP_SIGNALING_IDENTIFIER_OFFSET] ==
		raw_interrupt_connection_identifier &&
	    connection_source_cid == L2CAP_LOCAL_CID_HID_INTERRUPT) {
		publish_active_reconnect(0xd9, L2CAP_PSM_HID_INTERRUPT,
			connection_result,
			connection_destination_cid);
		if (connection_result == 0 &&
		    connection_destination_cid >= L2CAP_FIRST_DYNAMIC_CID) {
			raw_interrupt_remote_cid = connection_destination_cid;
			__sync_synchronize();
		} else if (connection_result > 1) {
			active_reconnect_state = ACTIVE_RECONNECT_DISCONNECTING;
			active_reconnect_started = 0;
		}
	}
	int expected_raw_configuration = raw_acl_reconnect &&
		packet_handle == switch_acl_handle &&
		active_reconnect_state == ACTIVE_RECONNECT_CONTROL_PENDING &&
		configuration_request_packet &&
		configuration_destination_cid == L2CAP_LOCAL_CID_HID_CONTROL &&
		raw_control_remote_cid >= L2CAP_FIRST_DYNAMIC_CID;
	if (expected_raw_configuration) {
		int expected_options = signaling_command_length == 8 &&
			read_le16(data + L2CAP_SIGNALING_DATA_OFFSET + 2) == 0 &&
			data[L2CAP_SIGNALING_DATA_OFFSET + 4] ==
				L2CAP_CONFIGURATION_OPTION_MTU &&
			data[L2CAP_SIGNALING_DATA_OFFSET + 5] == sizeof(uint16_t) &&
			read_le16(data + L2CAP_SIGNALING_DATA_OFFSET + 6) ==
				L2CAP_HID_MTU;
		if (expected_options) {
			raw_control_config_identifier =
				data[L2CAP_SIGNALING_IDENTIFIER_OFFSET];
			__sync_synchronize();
			if (__sync_bool_compare_and_swap(&active_reconnect_state,
			    ACTIVE_RECONNECT_CONTROL_PENDING,
			    ACTIVE_RECONNECT_CONTROL_CONFIG_START)) {
				active_reconnect_started = ksceKernelGetSystemTimeLow();
				publish_active_reconnect(0xd4, L2CAP_PSM_HID_CONTROL,
					data[L2CAP_SIGNALING_IDENTIFIER_OFFSET],
					raw_control_remote_cid);
			}
		} else {
			publish_active_reconnect(0xd4, L2CAP_PSM_HID_CONTROL, -1,
				configuration_destination_cid);
			active_reconnect_state = ACTIVE_RECONNECT_DISCONNECTING;
			active_reconnect_started = 0;
		}
	}
	int expected_raw_interrupt_configuration = raw_acl_reconnect &&
		packet_handle == switch_acl_handle &&
		active_reconnect_state == ACTIVE_RECONNECT_RAW_INTERRUPT_PENDING &&
		configuration_request_packet &&
		configuration_destination_cid == L2CAP_LOCAL_CID_HID_INTERRUPT &&
		raw_interrupt_remote_cid >= L2CAP_FIRST_DYNAMIC_CID;
	if (expected_raw_interrupt_configuration) {
		int expected_options = signaling_command_length == 8 &&
			read_le16(data + L2CAP_SIGNALING_DATA_OFFSET + 2) == 0 &&
			data[L2CAP_SIGNALING_DATA_OFFSET + 4] ==
				L2CAP_CONFIGURATION_OPTION_MTU &&
			data[L2CAP_SIGNALING_DATA_OFFSET + 5] == sizeof(uint16_t) &&
			read_le16(data + L2CAP_SIGNALING_DATA_OFFSET + 6) ==
				L2CAP_HID_MTU;
		if (expected_options) {
			raw_interrupt_config_identifier =
				data[L2CAP_SIGNALING_IDENTIFIER_OFFSET];
			__sync_synchronize();
			if (__sync_bool_compare_and_swap(&active_reconnect_state,
			    ACTIVE_RECONNECT_RAW_INTERRUPT_PENDING,
			    ACTIVE_RECONNECT_RAW_INTERRUPT_CONFIG_START)) {
				active_reconnect_started = ksceKernelGetSystemTimeLow();
				publish_active_reconnect(0xda, L2CAP_PSM_HID_INTERRUPT,
					data[L2CAP_SIGNALING_IDENTIFIER_OFFSET],
					raw_interrupt_remote_cid);
			}
		} else {
			publish_active_reconnect(0xda, L2CAP_PSM_HID_INTERRUPT, -1,
				configuration_destination_cid);
			active_reconnect_state = ACTIVE_RECONNECT_DISCONNECTING;
			active_reconnect_started = 0;
		}
	}
	if (connection_response_packet && connection_result > 1 &&
	    ((control_cid && connection_source_cid == control_cid) ||
	     (interrupt_cid && connection_source_cid == interrupt_cid)) &&
	    active_reconnect_state != ACTIVE_RECONNECT_IDLE) {
		active_reconnect_state = ACTIVE_RECONNECT_DISCONNECTING;
		active_reconnect_started = 0;
	}
	int hid_output = l2cap_packet && interrupt_cid &&
		l2cap_cid == interrupt_cid && l2cap_length >= 2 &&
		(unsigned int)length >=
			(unsigned int)RAW_ACL_HEADER_SIZE + l2cap_length &&
		data[RAW_ACL_HEADER_SIZE] == PROCON_HID_DATA_OUTPUT;
	int hid_request = find_connection && find_channel && l2cap_connection_response &&
		l2cap_packet && l2cap_cid == L2CAP_SIGNALING_CID &&
		length >= 16 && data[L2CAP_SIGNALING_COMMAND_OFFSET] ==
			L2CAP_SIGNAL_CONNECTION_REQUEST &&
		signaling_command_length == 4;
	uint16_t psm = hid_request ?
		read_le16(data + L2CAP_SIGNALING_DATA_OFFSET) : 0;
	hid_request &= psm == L2CAP_PSM_HID_CONTROL ||
		psm == L2CAP_PSM_HID_INTERRUPT;
	uint16_t handle = hid_request ? packet_handle : 0;
	uint8_t identifier = hid_request ?
		data[L2CAP_SIGNALING_IDENTIFIER_OFFSET] : 0;
	if (hid_request) {
		/* If the Switch opens HID first, the passive path won this ACL.  Do
		 * not race it with our delayed active PSM 0x11 request. */
		int passive_won = __sync_bool_compare_and_swap(&active_reconnect_state,
			ACTIVE_RECONNECT_CONTROL_START, ACTIVE_RECONNECT_IDLE);
		passive_won |= __sync_bool_compare_and_swap(&active_reconnect_state,
			ACTIVE_RECONNECT_ACL_PENDING, ACTIVE_RECONNECT_IDLE);
		passive_won |= __sync_bool_compare_and_swap(&active_reconnect_state,
			ACTIVE_RECONNECT_PASSIVE_WAIT, ACTIVE_RECONNECT_IDLE);
		passive_won |= __sync_bool_compare_and_swap(&active_reconnect_state,
			ACTIVE_RECONNECT_PASSIVE_ACL_WAIT, ACTIVE_RECONNECT_IDLE);
		passive_won |= __sync_bool_compare_and_swap(&active_reconnect_state,
			ACTIVE_RECONNECT_RETRY_WAIT, ACTIVE_RECONNECT_IDLE);
		if (passive_won)
			active_reconnect_attempts = 0;
		switch_acl_handle = handle;
		pending_hid_connection = NULL;
		pending_hid_channel = NULL;
		keepalive_active = 0;
	}

	int original_result = TAI_CONTINUE(int, acl_event_ref, context, data, length);
	if (send_initial_after_config)
		send_initial_input_after_config(interrupt_configured_at);
	if (connection_response_packet && packet_handle == switch_acl_handle &&
	    connection_result == 0 &&
	    connection_source_cid &&
	    active_reconnect_state == ACTIVE_RECONNECT_INTERRUPT_START &&
	    find_channel && switch_connection) {
		/* find_channel kind 5 looks up an existing channel by local CID.
		 * SceBt's control-channel callback allocates and starts PSM 0x13
		 * itself; adopting that channel avoids allocating a duplicate. */
		void *channel = find_channel(switch_connection, 5,
			connection_source_cid);
		uint16_t channel_psm = channel ? *(const uint16_t *)
			((const uint8_t *)channel + SCEBT_CHANNEL_PSM_OFFSET) : 0;
		if (channel && channel_psm == L2CAP_PSM_HID_INTERRUPT) {
			hid_interrupt_channel = channel;
			active_reconnect_state = ACTIVE_RECONNECT_INTERRUPT_PENDING;
			active_reconnect_started = ksceKernelGetSystemTimeLow();
			uint32_t channel_flags = *(const uint32_t *)
				((const uint8_t *)channel + SCEBT_CHANNEL_FLAGS_OFFSET);
			publish_active_reconnect(0xe6, L2CAP_PSM_HID_INTERRUPT,
				(int)(channel_flags & 0xffffU), connection_source_cid);
		}
	}
	if (hid_request) {
		/* SceBt sends an L2CAP "pending" response before returning here.
		 * Sending the final accept from this receive callback re-enters its
		 * signaling path while the ACL dispatcher still owns internal state.
		 * Queue it for the plugin worker so the receive stack can unwind first. */
		unsigned int slot = psm == L2CAP_PSM_HID_CONTROL ? 0U : 1U;
		unsigned int bit = 1U << slot;
		pending_l2cap_handle[slot] = handle;
		pending_l2cap_identifier[slot] = identifier;
		pending_l2cap_context[slot] = context;
		__sync_synchronize();
		__sync_fetch_and_or(&l2cap_response_pending, bit);
		uint8_t probe[8] = { 0x54, 1, (uint8_t)psm,
			(uint8_t)(psm >> 8), identifier, (uint8_t)handle,
			(uint8_t)(handle >> 8), 0 };
		publish(6, probe, sizeof(probe));
	}
	if (hid_output && switch_connection && hid_interrupt_channel && l2cap_send) {
		/* Modern Switch firmware can answer the initial empty input report
		 * with an A2 00 placeholder before sending its first A2 01
		 * subcommand.  Keep the one-Hz bootstrap alive across placeholders. */
		const uint8_t *output = data + RAW_ACL_HEADER_SIZE;
		int subcommand_output = l2cap_length >= PROCON_OUTPUT_DATA_OFFSET &&
			output[1] == PROCON_OUTPUT_REPORT_SUBCOMMAND;
		uint8_t command = subcommand_output ?
			output[PROCON_OUTPUT_SUBCOMMAND_OFFSET] : 0;
		if (!subcommands_started)
			bootstrap_output_seen = 1;
		if (subcommand_output) {
			subcommands_started = 1;
			if (command == PROCON_SUBCOMMAND_SET_INPUT_MODE)
				continuous_input_enabled = 1;
			if (command == PROCON_SUBCOMMAND_TRIGGER_BUTTONS ||
			    command == PROCON_SUBCOMMAND_SET_PLAYER_LIGHTS)
				pairing_confirmed = 1;
		} else if (!subcommands_started) {
			uint8_t probe[6] = { 0x58, output[1],
				(uint8_t)l2cap_length, (uint8_t)(l2cap_length >> 8),
				(uint8_t)packet_handle, (uint8_t)(packet_handle >> 8) };
			publish(10, probe, sizeof(probe));
		}
		uint8_t reply[PROCON_REPLY_SIZE];
		size_t reply_length = procon_handle_output(&procon_state, output,
			l2cap_length, procon_timer++, reply);
		if (reply_length > 1) {
			if (command == PROCON_SUBCOMMAND_DEVICE_INFO &&
			    reply_length >= 26) {
				uint8_t identity_probe[10] = { 0x5E,
					reply[16], reply[17], reply[18],
					reply[20], reply[21], reply[22], reply[23],
					reply[24], reply[25] };
				publish(10, identity_probe, sizeof(identity_probe));
			}
			uint16_t remote_cid = *(const uint16_t *)
				((const uint8_t *)hid_interrupt_channel +
					SCEBT_CHANNEL_REMOTE_CID_OFFSET);
			input_send_ready = 0;
			int send_result = l2cap_send(switch_connection, remote_cid,
				l2cap_raw_format, reply[0], reply + 1, reply_length - 1);
			if (send_result < 0)
				input_send_ready = 1;
			uint8_t probe[4] = { 0x52,
				reply[PROCON_REPLY_SUBCOMMAND_OFFSET],
				(uint8_t)send_result,
				(uint8_t)(send_result >> 8) };
			publish(6, probe, sizeof(probe));
		}
	}
	return original_result;
}

static void handle_l2cap_response(void)
{
	/* Control and interrupt requests commonly arrive back-to-back.  Drain
	 * both independent bits so one request cannot overwrite the other. */
	for (;;) {
		unsigned int pending = l2cap_response_pending;
		if (!pending)
			return;
		unsigned int slot = pending & 1U ? 0U : 1U;
		unsigned int bit = 1U << slot;
		__sync_synchronize();
		uint16_t psm = slot == 0 ? L2CAP_PSM_HID_CONTROL :
			L2CAP_PSM_HID_INTERRUPT;
		uint16_t handle = pending_l2cap_handle[slot];
		uint8_t identifier = pending_l2cap_identifier[slot];
		void *context = pending_l2cap_context[slot];
		__sync_fetch_and_and(&l2cap_response_pending, ~bit);

		void *connection = switch_connection;
		void *channel = slot == 0 ? hid_control_channel :
			hid_interrupt_channel;
		if ((!connection || !is_switch_connection(connection)) && find_connection)
			connection = find_connection(context, 2, handle, 0);
		if (!channel && connection && is_switch_connection(connection) && find_channel)
			channel = find_channel(connection, 20, psm);
		uint16_t local_cid = channel ? *(const uint16_t *)
			((const uint8_t *)channel + SCEBT_CHANNEL_LOCAL_CID_OFFSET) : 0;
		uint16_t remote_cid = channel ? *(const uint16_t *)
			((const uint8_t *)channel + SCEBT_CHANNEL_REMOTE_CID_OFFSET) : 0;
		uint8_t started[8] = { 0x54, 2, (uint8_t)psm,
			(uint8_t)(psm >> 8), identifier, (uint8_t)local_cid,
			(uint8_t)(local_cid >> 8), channel != NULL };
		publish(6, started, sizeof(started));

		int response_result = channel && l2cap_connection_response ?
			l2cap_connection_response(connection, channel, 0, 0) : -1;
		if (channel && response_result >= 0)
			keepalive_active = 1;
		else if (!l2cap_response_pending)
			keepalive_active = 0;
		uint8_t finished[10] = {
			0x53, (uint8_t)psm, (uint8_t)(psm >> 8), identifier,
			(uint8_t)local_cid, (uint8_t)(local_cid >> 8),
			(uint8_t)remote_cid, (uint8_t)(remote_cid >> 8),
			(uint8_t)response_result, (uint8_t)(response_result >> 8),
		};
		publish(6, finished, sizeof(finished));
	}
}

static uint16_t scale_vita_axis(uint8_t value, int invert)
{
	if (value >= 120 && value <= 136)
		return PROCON_STICK_CENTER;
	uint16_t scaled = (uint16_t)(((uint32_t)value * PROCON_STICK_MAX) /
		255U);
	return invert ? (uint16_t)(PROCON_STICK_MAX - scaled) : scaled;
}

static int stick_trace_changed(uint32_t old_sticks, uint32_t new_sticks)
{
	for (unsigned int shift = 0; shift < 32; shift += 8) {
		int old_axis = (uint8_t)(old_sticks >> shift);
		int new_axis = (uint8_t)(new_sticks >> shift);
		int difference = new_axis - old_axis;
		if (difference <= -8 || difference >= 8)
			return 1;
	}
	return 0;
}

static void update_battery_state(void)
{
	uint32_t now = ksceKernelGetSystemTimeLow();
	if (last_battery_poll_time &&
	    now - last_battery_poll_time < BATTERY_POLL_INTERVAL_US)
		return;
	last_battery_poll_time = now;
	int percent = kscePowerGetBatteryLifePercent();
	if (percent < 0)
		return;
	if (percent > 100)
		percent = 100;
	int charging = kscePowerIsBatteryCharging() > 0;
	procon_set_battery(&procon_state, percent, charging);
	if (percent != last_battery_percent || charging != last_battery_charging) {
		uint8_t probe[4] = { 0x4A, (uint8_t)percent,
			(uint8_t)charging, procon_state.battery_level };
		publish(10, probe, sizeof(probe));
		last_battery_percent = percent;
		last_battery_charging = charging;
	}
}

static void update_controller_input(void)
{
	expire_controller_heartbeat();
	update_battery_state();
	uint8_t mapped[PROCON_BUTTON_BYTES] = { 0, 0, 0 };
	uint16_t lx = PROCON_STICK_CENTER;
	uint16_t ly = PROCON_STICK_CENTER;
	uint16_t rx = PROCON_STICK_CENTER;
	uint16_t ry = PROCON_STICK_CENTER;
	uint32_t raw = 0;
	uint32_t sticks = 0x80808080U;
	uint8_t touch_buttons = 0;
	if (controller_app_active) {
		unsigned int sequence;
		for (;;) {
			sequence = controller_input_sequence;
			if (sequence & 1U)
				continue;
			raw = controller_vita_buttons;
			sticks = controller_vita_sticks;
			touch_buttons = controller_touch_buttons;
			__sync_synchronize();
			if (sequence == controller_input_sequence)
				break;
		}
		/* Nintendo face buttons are named by physical position. */
		if (raw & SCE_CTRL_SQUARE)
			mapped[PROCON_BUTTON_BYTE_RIGHT] |= PROCON_RIGHT_BUTTON_Y;
		if (raw & SCE_CTRL_TRIANGLE)
			mapped[PROCON_BUTTON_BYTE_RIGHT] |= PROCON_RIGHT_BUTTON_X;
		if (raw & SCE_CTRL_CROSS)
			mapped[PROCON_BUTTON_BYTE_RIGHT] |= PROCON_RIGHT_BUTTON_B;
		if (raw & SCE_CTRL_CIRCLE)
			mapped[PROCON_BUTTON_BYTE_RIGHT] |= PROCON_RIGHT_BUTTON_A;
		int vita_r = raw & (SCE_CTRL_RTRIGGER | SCE_CTRL_R1);
		int vita_l = raw & (SCE_CTRL_LTRIGGER | SCE_CTRL_L1);
		int touch_zr = touch_buttons & VITA_NS_TOUCH_ZR;
		int touch_zl = touch_buttons & VITA_NS_TOUCH_ZL;
		if (touch_buttons & VITA_NS_TOUCH_SWAP_RIGHT) {
			if (vita_r)
				mapped[PROCON_BUTTON_BYTE_RIGHT] |= PROCON_RIGHT_BUTTON_ZR;
			if (touch_zr)
				mapped[PROCON_BUTTON_BYTE_RIGHT] |= PROCON_RIGHT_BUTTON_R;
		} else {
			if (vita_r)
				mapped[PROCON_BUTTON_BYTE_RIGHT] |= PROCON_RIGHT_BUTTON_R;
			if (touch_zr)
				mapped[PROCON_BUTTON_BYTE_RIGHT] |= PROCON_RIGHT_BUTTON_ZR;
		}
		if (raw & SCE_CTRL_SELECT)
			mapped[PROCON_BUTTON_BYTE_SHARED] |= PROCON_SHARED_BUTTON_MINUS;
		if (raw & SCE_CTRL_START)
			mapped[PROCON_BUTTON_BYTE_SHARED] |= PROCON_SHARED_BUTTON_PLUS;
		if (raw & SCE_CTRL_R3)
			mapped[PROCON_BUTTON_BYTE_SHARED] |=
				PROCON_SHARED_BUTTON_R_STICK;
		if (raw & SCE_CTRL_L3)
			mapped[PROCON_BUTTON_BYTE_SHARED] |=
				PROCON_SHARED_BUTTON_L_STICK;
		if (raw & SCE_CTRL_DOWN)
			mapped[PROCON_BUTTON_BYTE_LEFT] |= PROCON_LEFT_BUTTON_DOWN;
		if (raw & SCE_CTRL_UP)
			mapped[PROCON_BUTTON_BYTE_LEFT] |= PROCON_LEFT_BUTTON_UP;
		if (raw & SCE_CTRL_RIGHT)
			mapped[PROCON_BUTTON_BYTE_LEFT] |= PROCON_LEFT_BUTTON_RIGHT;
		if (raw & SCE_CTRL_LEFT)
			mapped[PROCON_BUTTON_BYTE_LEFT] |= PROCON_LEFT_BUTTON_LEFT;
		if (touch_buttons & VITA_NS_TOUCH_SWAP_LEFT) {
			if (vita_l)
				mapped[PROCON_BUTTON_BYTE_LEFT] |= PROCON_LEFT_BUTTON_ZL;
			if (touch_zl)
				mapped[PROCON_BUTTON_BYTE_LEFT] |= PROCON_LEFT_BUTTON_L;
		} else {
			if (vita_l)
				mapped[PROCON_BUTTON_BYTE_LEFT] |= PROCON_LEFT_BUTTON_L;
			if (touch_zl)
				mapped[PROCON_BUTTON_BYTE_LEFT] |= PROCON_LEFT_BUTTON_ZL;
		}
		if (touch_buttons & VITA_NS_TOUCH_L3)
			mapped[PROCON_BUTTON_BYTE_SHARED] |=
				PROCON_SHARED_BUTTON_L_STICK;
		if (touch_buttons & VITA_NS_TOUCH_HOME)
			mapped[PROCON_BUTTON_BYTE_SHARED] |= PROCON_SHARED_BUTTON_HOME;
		if (touch_buttons & VITA_NS_TOUCH_CAPTURE)
			mapped[PROCON_BUTTON_BYTE_SHARED] |=
				PROCON_SHARED_BUTTON_CAPTURE;
		if (touch_buttons & VITA_NS_TOUCH_R3)
			mapped[PROCON_BUTTON_BYTE_SHARED] |=
				PROCON_SHARED_BUTTON_R_STICK;
		lx = scale_vita_axis((uint8_t)sticks, 0);
		ly = scale_vita_axis((uint8_t)(sticks >> 8), 1);
		rx = scale_vita_axis((uint8_t)(sticks >> 16), 0);
		ry = scale_vita_axis((uint8_t)(sticks >> 24), 1);
	}
	if (controller_app_active && continuous_input_enabled &&
	    !pairing_confirmed && !procon_state.player_lights) {
		mapped[PROCON_BUTTON_BYTE_RIGHT] |= PROCON_RIGHT_BUTTON_R;
		mapped[PROCON_BUTTON_BYTE_LEFT] |= PROCON_LEFT_BUTTON_L;
	}
	if (raw != previous_vita_buttons ||
	    stick_trace_changed(previous_vita_sticks, sticks) ||
	    touch_buttons != previous_touch_buttons) {
		uint8_t probe[13] = { 0x49,
			mapped[PROCON_BUTTON_BYTE_RIGHT],
			mapped[PROCON_BUTTON_BYTE_SHARED],
			mapped[PROCON_BUTTON_BYTE_LEFT],
			(uint8_t)raw, (uint8_t)(raw >> 8),
			(uint8_t)(raw >> 16), (uint8_t)(raw >> 24),
			(uint8_t)sticks, (uint8_t)(sticks >> 8),
			(uint8_t)(sticks >> 16), (uint8_t)(sticks >> 24),
			touch_buttons };
		publish(10, probe, sizeof(probe));
		previous_vita_buttons = raw;
		previous_vita_sticks = sticks;
		previous_touch_buttons = touch_buttons;
	}
	procon_set_input(&procon_state, mapped, lx, ly, rx, ry);
}

static int input_state_changed(void)
{
	return memcmp(last_sent_buttons, procon_state.buttons,
			sizeof(last_sent_buttons)) ||
		last_sent_lx != procon_state.lx || last_sent_ly != procon_state.ly ||
		last_sent_rx != procon_state.rx || last_sent_ry != procon_state.ry ||
		last_sent_battery_level != procon_state.battery_level;
}

static void raw_input_transport_complete(void *argument)
{
	(void)argument;
	raw_input_pending = 0;
	__sync_synchronize();
}

static void send_raw_connected_input(void)
{
	if (!raw_acl_reconnect || !switch_acl_handle ||
	    !raw_interrupt_remote_cid || !hci_transport_enqueue ||
	    !input_send_ready || raw_input_pending)
		return;

	update_controller_input();
	uint32_t now = ksceKernelGetSystemTimeLow();
	uint32_t age = now - last_input_report_time;
	int changed = input_state_changed();
	if (last_input_report_time &&
	    (age < RAW_INPUT_REPORT_INTERVAL_US ||
	     (!changed && age < INPUT_KEEPALIVE_US)))
		return;

	static uint8_t packets[2][RAW_ACL_HEADER_SIZE + PROCON_INPUT_SIZE]
		__attribute__((aligned(4)));
	unsigned int report_index = raw_connected_input_reports++;
	uint8_t *packet = packets[report_index & 1U];
	uint16_t handle = switch_acl_handle;
	uint16_t remote_cid = raw_interrupt_remote_cid;
	prepare_acl_l2cap_packet(packet, sizeof(packets[0]), handle, remote_cid,
		PROCON_INPUT_SIZE);
	if (procon_make_input(&procon_state, procon_timer++,
	    packet + RAW_ACL_HEADER_SIZE) !=
	    PROCON_INPUT_SIZE)
		return;

	input_send_ready = 0;
	raw_input_pending = 1;
	__sync_synchronize();
	last_input_report_time = now;
	int result = hci_transport_enqueue(HCI_PACKET_TYPE_ACL_DATA, packet,
		sizeof(packets[0]),
		raw_input_transport_complete, NULL);
	if (result < 0) {
		raw_input_pending = 0;
		input_send_ready = 1;
	} else {
		memcpy(last_sent_buttons, procon_state.buttons,
			sizeof(last_sent_buttons));
		last_sent_lx = procon_state.lx;
		last_sent_ly = procon_state.ly;
		last_sent_rx = procon_state.rx;
		last_sent_ry = procon_state.ry;
		last_sent_battery_level = procon_state.battery_level;
	}
	if (report_index < 4U || !(report_index & 0xffU))
		publish_active_reconnect(RAW_TRACE_INPUT_REPORT,
			(uint16_t)report_index,
			result, remote_cid);
}

static int sender_worker(SceSize argc, void *args)
{
	(void)argc;
	(void)args;
	while (!sender_stop_requested) {
		if (!sender_request_pending) {
			ksceKernelDelayThread(2000);
			continue;
		}
		__sync_synchronize();
		void *connection = sender_connection;
		uint16_t remote_cid = sender_remote_cid;
		unsigned int length = sender_report_length;
		uint8_t report[HID_BOOTSTRAP_REPORT_SIZE];
		if (length > sizeof(report))
			length = sizeof(report);
		memcpy(report, sender_report, length);
		sender_blocked = 1;
		__sync_synchronize();
		sender_request_pending = 0;
		int result = connection && remote_cid && length > 1 && l2cap_send ?
			l2cap_send(connection, remote_cid, l2cap_raw_format,
				report[0], report + 1, length - 1) : -1;
		if (result < 0)
			input_send_ready = 1;
		sender_blocked = 0;
		uint8_t probe[6] = { 0x51, (uint8_t)result,
			(uint8_t)(result >> 8), (uint8_t)remote_cid,
			(uint8_t)(remote_cid >> 8), (uint8_t)length };
		publish(6, probe, sizeof(probe));
	}
	return 0;
}

static void send_input(void)
{
	if (raw_input_active && raw_acl_reconnect &&
	    active_reconnect_state == ACTIVE_RECONNECT_IDLE &&
	    subcommands_started && continuous_input_enabled) {
		send_raw_connected_input();
		return;
	}
	if (hid_configured != HID_CONFIGURED_ALL || !keepalive_active ||
	    !switch_connection ||
	    !hid_interrupt_channel || !l2cap_send ||
	    !input_send_ready || sender_request_pending || sender_blocked ||
	    (subcommands_started && !continuous_input_enabled))
		return;
	/* Physical input is sampled only while VITANSPAD is foreground. */
	update_controller_input();
	uint32_t now = ksceKernelGetSystemTimeLow();
	uint32_t age = now - last_input_report_time;
	int changed = input_state_changed();
	/* joycontrol/NXBT wait at low rate for the first Switch output report.
	 * Once initialized, transmit changes immediately and otherwise use a
	 * one-second keepalive instead of flooding SceBt's blocking ACL queue. */
	if ((!subcommands_started && last_input_report_time &&
		    age < (bootstrap_output_seen ?
			HID_INITIALIZED_REPORT_INTERVAL_US : INPUT_KEEPALIVE_US)) ||
		   (subcommands_started && !changed && age < INPUT_KEEPALIVE_US)) {
		return;
	}
	uint8_t report[HID_BOOTSTRAP_REPORT_SIZE];
	size_t report_length;
	if (!subcommands_started) {
		/* NXBT's modern-firmware reconnect path sends one report-ID-zero
		 * prompt, then regular A1 30 reports at one Hz until the first Switch
		 * output.  That also preserves the L+R state which initiated paging. */
		if (!bootstrap_output_seen &&
		    bootstrap_input_reports >= HID_BOOTSTRAP_REPORT_COUNT)
			return;
		if (!bootstrap_input_reports) {
			memset(report, 0, PROCON_INPUT_SIZE);
			report[0] = PROCON_HID_DATA_INPUT;
			report_length = PROCON_INPUT_SIZE;
			uint8_t probe[8] = { 0x57, (uint8_t)report_length,
				report[0], report[1], report[2], report[3],
				(uint8_t)switch_acl_handle,
				(uint8_t)(switch_acl_handle >> 8) };
			publish(10, probe, sizeof(probe));
		} else {
			report_length = procon_make_input(&procon_state,
				procon_timer++, report);
			if (bootstrap_input_reports == 1) {
				uint8_t probe[8] = { 0x59, (uint8_t)report_length,
					report[0], report[1], report[2], report[3],
					(uint8_t)switch_acl_handle,
					(uint8_t)(switch_acl_handle >> 8) };
				publish(10, probe, sizeof(probe));
			}
		}
		bootstrap_input_reports++;
	} else {
		report_length = procon_make_input(&procon_state, procon_timer++, report);
	}
	uint16_t remote_cid = *(const uint16_t *)
		((const uint8_t *)hid_interrupt_channel +
			SCEBT_CHANNEL_REMOTE_CID_OFFSET);
	if (!remote_cid || report_length > sizeof(sender_report))
		return;
	input_send_ready = 0;
	sender_connection = switch_connection;
	sender_remote_cid = remote_cid;
	sender_report_length = report_length;
	memcpy(sender_report, report, report_length);
	memcpy(last_sent_buttons, procon_state.buttons, sizeof(last_sent_buttons));
	last_sent_lx = procon_state.lx;
	last_sent_ly = procon_state.ly;
	last_sent_rx = procon_state.rx;
	last_sent_ry = procon_state.ry;
	last_sent_battery_level = procon_state.battery_level;
	last_input_report_time = now;
	__sync_synchronize();
	sender_request_pending = 1;
}

static void handle_reconnect_request(void)
{
	if (!reconnect_requested)
		return;
	reconnect_requested = 0;
	if (pairing_reset_requested || pairing_reset_state ||
	    pairing_discoverable_active) {
		uint8_t blocked[8] = { 0x43, 0xFD, 0xFF, 0xFF, 0xFF,
			(uint8_t)switch_acl_handle,
			(uint8_t)(switch_acl_handle >> 8), 1 };
		publish(10, blocked, sizeof(blocked));
		return;
	}
	if (active_reconnect_state != ACTIVE_RECONNECT_IDLE)
		return;
	int result = -1;
	if (switch_acl_handle && hci_command) {
		uint16_t handle = switch_acl_handle;
		keepalive_active = 0;
		input_send_ready = 0;
		force_disconnect = 1;
		result = hci_command(hci_context, HCI_OPCODE_DISCONNECT, "21",
			handle, HCI_REASON_REMOTE_USER_TERMINATED_CONNECTION);
		force_disconnect = 0;
		active_reconnect_state = ACTIVE_RECONNECT_DISCONNECTING;
		active_reconnect_started = ksceKernelGetSystemTimeLow();
	} else if (!switch_acl_handle && hci_command &&
	    hci_create_connection_format) {
		/* A bonded reconnect must not advertise as a new discoverable device:
		 * that makes Change Grip/Order register the same Vita again.  Keep only
		 * page scan enabled while giving the saved Switch an inbound window.
		 * With no saved bond, retain the original first-pairing behavior. */
		int bonded = bond_state_has_saved_bond();
		int scan_result = bonded ? set_scan_and_wait(0) :
			force_discoverable_scan();
		uint8_t scan_probe[4] = { 0x56, bonded ? 3 : 1,
			(uint8_t)scan_result,
			(uint8_t)(scan_result >> 8) };
		publish(10, scan_probe, sizeof(scan_probe));
		if (!bonded && scan_result >= 0) {
			active_reconnect_scan_active = 1;
			active_reconnect_scan_started = ksceKernelGetSystemTimeLow();
		}
		result = scan_result;
		active_reconnect_started = ksceKernelGetSystemTimeLow();
		active_reconnect_state = result >= 0 ?
			ACTIVE_RECONNECT_PASSIVE_WAIT : ACTIVE_RECONNECT_RETRY_WAIT;
	}
	uint8_t probe[8] = { 0x43, (uint8_t)result,
		(uint8_t)(result >> 8), (uint8_t)(result >> 16),
		(uint8_t)(result >> 24), (uint8_t)switch_acl_handle,
		(uint8_t)(switch_acl_handle >> 8),
		(uint8_t)active_reconnect_state };
	publish(10, probe, sizeof(probe));
}

static void publish_active_reconnect(uint8_t phase, uint16_t psm, int result,
	uint16_t local_cid)
{
	uint8_t probe[10] = { 0x55, phase, (uint8_t)psm,
		(uint8_t)(psm >> 8), (uint8_t)result, (uint8_t)(result >> 8),
		(uint8_t)local_cid, (uint8_t)(local_cid >> 8),
		(uint8_t)active_reconnect_attempts,
		(uint8_t)active_reconnect_state };
	publish(10, probe, sizeof(probe));
}

static void reset_raw_control_channel(void)
{
	raw_control_remote_cid = 0;
	raw_control_config_identifier = 0;
	raw_control_request_identifier = 0;
	raw_interrupt_remote_cid = 0;
	raw_interrupt_connection_identifier = 0;
	raw_interrupt_config_identifier = 0;
	raw_interrupt_request_identifier = 0;
	raw_subcommand_request_length = 0;
	raw_init_script_stage = 0;
	raw_input_active = 0;
	raw_input_pending = 0;
	raw_connected_input_reports = 0;
}

static int connect_switch_peer(void)
{
	if (!hci_command || !hci_create_connection_format)
		return -1;
	uint32_t address_low;
	uint16_t address_high;
	if (!bond_state_read_peer(&address_low, &address_high))
		return -2;
	/* Do not call SceBt's connect_remote wrapper here.  It is a HID-host path:
	 * after encryption it queries the Switch's SDP database and then asks to
	 * tear the ACL down because the console is not a remote HID device.  A
	 * controller reconnect, as used by NXBT/joycontrol, creates the baseband
	 * ACL and then opens HID 0x11/0x13 itself.  SceBt's normal HCI event handler
	 * still sees Connection Complete and owns the generic connection record. */
	const uint64_t address = address_low | ((uint64_t)address_high << 32);
	const uint16_t packet_type = 0x3318;
	const int page_scan_repetition_mode = 2;
	const int clock_offset = 0;
	const int allow_role_switch = 1;
	reset_raw_control_channel();
	int result = hci_command(hci_context, HCI_OPCODE_CREATE_CONNECTION,
		hci_create_connection_format, address, packet_type,
		page_scan_repetition_mode, clock_offset, allow_role_switch);
	raw_acl_reconnect = result >= 0;
	publish_active_reconnect(0x0b, (uint16_t)allow_role_switch, result,
		packet_type);
	return result;
}

static void raw_control_transport_complete(void *argument)
{
	unsigned int completion = __sync_add_and_fetch(&raw_transport_completions, 1);
	uint8_t probe[6] = { 0x55, RAW_TRACE_TRANSPORT_COMPLETE,
		(uint8_t)(uintptr_t)argument,
		(uint8_t)completion, (uint8_t)(completion >> 8),
		(uint8_t)active_reconnect_state };
	publish(10, probe, sizeof(probe));
}

static int send_raw_control_probe(void)
{
	if (!hci_transport_enqueue || !switch_acl_handle)
		return -1;

	/* Offset 0x106D1 silently drops ACL packets whose handle has no SceBt
	 * connection object.  Submit exactly one packet through the common HCI
	 * transport queue below that lookup.  Use our own completion callback so
	 * SceBt does not consume bytes from its unrelated native L2CAP ring.  This
	 * diagnostic deliberately does not modify SceBt's ACL credit counter and
	 * remains limited to a bounded signaling handshake with no HID traffic. */
	static uint8_t packets[4][RAW_L2CAP_CONNECTION_PACKET_SIZE]
		__attribute__((aligned(4)));
	unsigned int slot = (unsigned int)active_reconnect_attempts & 3U;
	uint8_t *packet = packets[slot];
	uint16_t handle = switch_acl_handle;
	const uint16_t local_cid = L2CAP_LOCAL_CID_HID_CONTROL;
	uint8_t identifier = (uint8_t)(RAW_CONTROL_CONNECTION_IDENTIFIER_BASE +
		slot);
	prepare_l2cap_signaling_packet(packet, sizeof(packets[0]), handle,
		L2CAP_SIGNAL_CONNECTION_REQUEST, identifier,
		L2CAP_CONNECTION_REQUEST_DATA_SIZE);
	write_le16(packet + L2CAP_SIGNALING_DATA_OFFSET,
		L2CAP_PSM_HID_CONTROL);
	write_le16(packet + L2CAP_SIGNALING_DATA_OFFSET + 2, local_cid);
	int result = hci_transport_enqueue(HCI_PACKET_TYPE_ACL_DATA, packet,
		sizeof(packets[0]),
		raw_control_transport_complete,
		(void *)(uintptr_t)(active_reconnect_attempts & 0xff));
	publish_active_reconnect(0xd1, L2CAP_PSM_HID_CONTROL, result, local_cid);
	return result;
}

static int send_raw_control_configuration(void)
{
	if (!hci_transport_enqueue || !switch_acl_handle ||
	    !raw_control_remote_cid || !raw_control_config_identifier)
		return -1;

	static uint8_t responses[4][RAW_L2CAP_CONFIGURATION_RESPONSE_PACKET_SIZE]
		__attribute__((aligned(4)));
	static uint8_t requests[4][RAW_L2CAP_CONFIGURATION_REQUEST_PACKET_SIZE]
		__attribute__((aligned(4)));
	unsigned int slot = (unsigned int)active_reconnect_attempts & 3U;
	uint16_t handle = switch_acl_handle;
	uint16_t remote_cid = raw_control_remote_cid;
	uint8_t *response = responses[slot];
	uint8_t *request = requests[slot];
	uint8_t request_identifier = (uint8_t)(
		RAW_CONTROL_CONFIGURATION_IDENTIFIER_BASE + slot);

	/* Accept the Switch's MTU 0x02A0 request.  Configuration Response SCID is
	 * the requester's local endpoint, which is the Switch CID returned by its
	 * successful Connection Response. */
	prepare_l2cap_configuration_packets(response, sizeof(responses[0]),
		request, sizeof(requests[0]), handle, remote_cid,
		raw_control_config_identifier, request_identifier);

	raw_control_request_identifier = request_identifier;
	__sync_synchronize();
	int response_result = hci_transport_enqueue(HCI_PACKET_TYPE_ACL_DATA,
		response,
		sizeof(responses[0]), raw_control_transport_complete,
		(void *)(uintptr_t)(0x80U | (active_reconnect_attempts & 0x0f)));
	publish_active_reconnect(0xd5, L2CAP_PSM_HID_CONTROL, response_result,
		remote_cid);
	if (response_result < 0)
		return response_result;
	int request_result = hci_transport_enqueue(HCI_PACKET_TYPE_ACL_DATA,
		request,
		sizeof(requests[0]), raw_control_transport_complete,
		(void *)(uintptr_t)(0x90U | (active_reconnect_attempts & 0x0f)));
	publish_active_reconnect(0xd6, L2CAP_PSM_HID_CONTROL, request_result,
		remote_cid);
	return request_result;
}

static int send_raw_interrupt_probe(void)
{
	if (!hci_transport_enqueue || !switch_acl_handle)
		return -1;

	static uint8_t packets[4][RAW_L2CAP_CONNECTION_PACKET_SIZE]
		__attribute__((aligned(4)));
	unsigned int slot = (unsigned int)active_reconnect_attempts & 3U;
	uint8_t *packet = packets[slot];
	uint16_t handle = switch_acl_handle;
	const uint16_t local_cid = L2CAP_LOCAL_CID_HID_INTERRUPT;
	uint8_t identifier = (uint8_t)(RAW_INTERRUPT_CONNECTION_IDENTIFIER_BASE +
		slot);
	prepare_l2cap_signaling_packet(packet, sizeof(packets[0]), handle,
		L2CAP_SIGNAL_CONNECTION_REQUEST, identifier,
		L2CAP_CONNECTION_REQUEST_DATA_SIZE);
	write_le16(packet + L2CAP_SIGNALING_DATA_OFFSET,
		L2CAP_PSM_HID_INTERRUPT);
	write_le16(packet + L2CAP_SIGNALING_DATA_OFFSET + 2, local_cid);
	raw_interrupt_connection_identifier = identifier;
	__sync_synchronize();
	int result = hci_transport_enqueue(HCI_PACKET_TYPE_ACL_DATA, packet,
		sizeof(packets[0]),
		raw_control_transport_complete,
		(void *)(uintptr_t)(0xa0U | (active_reconnect_attempts & 0x0f)));
	publish_active_reconnect(0xd8, L2CAP_PSM_HID_INTERRUPT, result,
		local_cid);
	return result;
}

static int send_raw_interrupt_configuration(void)
{
	if (!hci_transport_enqueue || !switch_acl_handle ||
	    !raw_interrupt_remote_cid || !raw_interrupt_config_identifier)
		return -1;

	static uint8_t responses[4][RAW_L2CAP_CONFIGURATION_RESPONSE_PACKET_SIZE]
		__attribute__((aligned(4)));
	static uint8_t requests[4][RAW_L2CAP_CONFIGURATION_REQUEST_PACKET_SIZE]
		__attribute__((aligned(4)));
	unsigned int slot = (unsigned int)active_reconnect_attempts & 3U;
	uint16_t handle = switch_acl_handle;
	uint16_t remote_cid = raw_interrupt_remote_cid;
	uint8_t *response = responses[slot];
	uint8_t *request = requests[slot];
	uint8_t request_identifier = (uint8_t)(
		RAW_INTERRUPT_CONFIGURATION_IDENTIFIER_BASE + slot);

	prepare_l2cap_configuration_packets(response, sizeof(responses[0]),
		request, sizeof(requests[0]), handle, remote_cid,
		raw_interrupt_config_identifier, request_identifier);

	raw_interrupt_request_identifier = request_identifier;
	__sync_synchronize();
	int response_result = hci_transport_enqueue(HCI_PACKET_TYPE_ACL_DATA,
		response,
		sizeof(responses[0]), raw_control_transport_complete,
		(void *)(uintptr_t)(0xb0U | (active_reconnect_attempts & 0x0f)));
	publish_active_reconnect(0xdb, L2CAP_PSM_HID_INTERRUPT,
		response_result, remote_cid);
	if (response_result < 0)
		return response_result;
	int request_result = hci_transport_enqueue(HCI_PACKET_TYPE_ACL_DATA,
		request,
		sizeof(requests[0]), raw_control_transport_complete,
		(void *)(uintptr_t)(0xc0U | (active_reconnect_attempts & 0x0f)));
	publish_active_reconnect(0xdc, L2CAP_PSM_HID_INTERRUPT,
		request_result, remote_cid);
	return request_result;
}

static int send_raw_initial_input(uint8_t phase, uint8_t timer)
{
	if (!hci_transport_enqueue || !switch_acl_handle ||
	    !raw_interrupt_remote_cid)
		return -1;

	/* Send the same 50-byte A1 30 bootstrap used by the proven native-channel
	 * path.  The raw device-info experiment permits exactly this one input
	 * report before waiting for the Switch's first subcommand. */
	static uint8_t packets[4][RAW_ACL_HEADER_SIZE + PROCON_INPUT_SIZE]
		__attribute__((aligned(4)));
	unsigned int slot = (unsigned int)active_reconnect_attempts & 3U;
	uint8_t *packet = packets[slot];
	uint16_t handle = switch_acl_handle;
	uint16_t remote_cid = raw_interrupt_remote_cid;
	prepare_acl_l2cap_packet(packet, sizeof(packets[0]), handle, remote_cid,
		PROCON_INPUT_SIZE);
	if (phase == RAW_TRACE_VALID_INPUT) {
		/* Once device info has been exchanged, a zero-filled standard report
		 * advertises battery level zero.  Build the bounded transition prompt
		 * through the normal protocol engine so battery, centered sticks, and
		 * vibrator state are all valid. */
		update_battery_state();
		if (procon_make_input(&procon_state, timer,
		    packet + RAW_ACL_HEADER_SIZE) !=
		    PROCON_INPUT_SIZE)
			return -2;
		publish_active_reconnect(RAW_TRACE_VALID_INPUT_STATE,
			packet[RAW_ACL_HEADER_SIZE + PROCON_INPUT_BATTERY_OFFSET],
			packet[RAW_ACL_HEADER_SIZE + PROCON_INPUT_VIBRATOR_OFFSET],
			remote_cid);
	} else {
		packet[RAW_ACL_HEADER_SIZE + PROCON_INPUT_DATA_TYPE_OFFSET] =
			PROCON_HID_DATA_INPUT;
		packet[RAW_ACL_HEADER_SIZE + PROCON_INPUT_REPORT_ID_OFFSET] =
			PROCON_INPUT_REPORT_STANDARD;
		packet[RAW_ACL_HEADER_SIZE + PROCON_INPUT_TIMER_OFFSET] = timer;
	}
	unsigned int completion_tag = phase == RAW_TRACE_VALID_INPUT ? 0x8fU :
		(0xd0U | (active_reconnect_attempts & 0x0f));
	int result = hci_transport_enqueue(HCI_PACKET_TYPE_ACL_DATA, packet,
		sizeof(packets[0]),
		raw_control_transport_complete,
		(void *)(uintptr_t)completion_tag);
	publish_active_reconnect(phase, L2CAP_PSM_HID_INTERRUPT, result,
		remote_cid);
	return result;
}

static int send_raw_subcommand_reply(uint8_t command, uint8_t phase)
{
	if (!hci_transport_enqueue || !switch_acl_handle ||
	    !raw_interrupt_remote_cid ||
	    raw_subcommand_request_length != PROCON_REPLY_SIZE ||
	    raw_subcommand_request[0] != PROCON_HID_DATA_OUTPUT ||
	    raw_subcommand_request[1] != PROCON_OUTPUT_REPORT_SUBCOMMAND ||
	    raw_subcommand_request[PROCON_OUTPUT_SUBCOMMAND_OFFSET] != command)
		return -1;

	static uint8_t packets[4][RAW_ACL_HEADER_SIZE + PROCON_REPLY_SIZE]
		__attribute__((aligned(4)));
	unsigned int slot = (unsigned int)active_reconnect_attempts & 3U;
	uint8_t *packet = packets[slot];
	uint16_t handle = switch_acl_handle;
	uint16_t remote_cid = raw_interrupt_remote_cid;
	prepare_acl_l2cap_packet(packet, sizeof(packets[0]), handle, remote_cid,
		PROCON_REPLY_SIZE);
	uint8_t reply_timer = procon_timer;
	size_t reply_length = procon_handle_output(&procon_state,
		raw_subcommand_request, raw_subcommand_request_length,
		reply_timer, packet + RAW_ACL_HEADER_SIZE);
	if (reply_length != PROCON_REPLY_SIZE)
		return -2;
	procon_timer++;
	unsigned int completion_tag;
	if (phase == 0xe8)
		completion_tag = 0x80U | (raw_init_script_stage & 0x0fU);
	else
		completion_tag = phase == 0xe0 ? 0xe0U :
			(phase == 0xe2 ? 0xf0U :
			 (phase == 0xe4 ? 0x60U : 0x70U));
	int result = hci_transport_enqueue(HCI_PACKET_TYPE_ACL_DATA, packet,
		sizeof(packets[0]),
		raw_control_transport_complete,
		(void *)(uintptr_t)(phase == 0xe8 ? completion_tag :
			(completion_tag | (active_reconnect_attempts & 0x0f))));
	uint16_t marker = phase == 0xe8 ?
		(uint16_t)(((uint16_t)raw_init_script_stage << 8) | command) :
		(uint16_t)command;
	publish_active_reconnect(phase, marker, result, remote_cid);
	return result;
}

static void request_active_disconnect(uint8_t phase)
{
	int result = 0;
	if (switch_acl_handle && hci_command) {
		uint16_t handle = switch_acl_handle;
		keepalive_active = 0;
		input_send_ready = 0;
		force_disconnect = 1;
		result = hci_command(hci_context, HCI_OPCODE_DISCONNECT, "21",
			handle, HCI_REASON_REMOTE_USER_TERMINATED_CONNECTION);
		force_disconnect = 0;
		active_reconnect_state = ACTIVE_RECONNECT_DISCONNECTING;
	} else {
		raw_acl_reconnect = 0;
		reset_raw_control_channel();
		active_reconnect_state = ACTIVE_RECONNECT_RETRY_WAIT;
	}
	active_reconnect_started = ksceKernelGetSystemTimeLow();
	publish_active_reconnect(phase, 0, result, 0);
}

static int start_active_hid_channel(uint16_t psm)
{
	/* kind 20 looks up the PSM's profile/listener channel.  It is correct only
	 * for control; the native control callback allocates and starts interrupt. */
	if (psm != L2CAP_PSM_HID_CONTROL)
		return -2;
	void *context = active_reconnect_context ? active_reconnect_context : hci_context;
	int use_deferred_channel = deferred_hid_connection &&
		deferred_hid_control_channel;
	void *connection = use_deferred_channel ? deferred_hid_connection :
		switch_connection;
	if ((!connection || !is_switch_connection(connection)) &&
	    find_connection && switch_acl_handle)
		connection = find_connection(context, 2, switch_acl_handle, 0);
	if (!connection && context != hci_context && find_connection &&
	    switch_acl_handle)
		connection = find_connection(hci_context, 2, switch_acl_handle, 0);
	if (!connection) {
		publish_active_reconnect(0xe1, psm, -1, 0);
		return -1;
	}
	if (!is_switch_connection(connection)) {
		publish_active_reconnect(0xe2, psm, -1, 0);
		return -1;
	}
	if (!l2cap_connect || (!use_deferred_channel &&
	    (!find_channel || !hid_channel_handler))) {
		publish_active_reconnect(0xe3, psm, -1, 0);
		return -1;
	}
	/* Reuse the exact native channel whose premature connect was deferred.
	 * The normal fallback allocates from the profile listener as before. */
	void *channel = use_deferred_channel ? deferred_hid_control_channel :
		find_channel(connection, 20, psm);
	if (!channel) {
		publish_active_reconnect(0xe4, psm, -1, 0);
		return -1;
	}
	uint16_t local_cid = *(const uint16_t *)
		((const uint8_t *)channel + SCEBT_CHANNEL_LOCAL_CID_OFFSET);
	uint32_t channel_flags = *(const uint32_t *)
		((const uint8_t *)channel + SCEBT_CHANNEL_FLAGS_OFFSET);
	publish_active_reconnect(0xe5, psm, (int)(channel_flags & 0xffffU),
		local_cid);
	void *previous_connection = switch_connection;
	void *previous_channel = psm == L2CAP_PSM_HID_CONTROL ?
		hid_control_channel :
		hid_interrupt_channel;
	active_hid_start_connection = connection;
	active_hid_start_channel = channel;
	switch_connection = connection;
	if (psm == L2CAP_PSM_HID_CONTROL)
		hid_control_channel = channel;
	else
		hid_interrupt_channel = channel;
	active_hid_start_call = 1;
	__sync_synchronize();
	int result;
	if (use_deferred_channel) {
		active_l2cap_connect_call = 1;
		__sync_synchronize();
		result = l2cap_connect(connection, channel);
		active_l2cap_connect_call = 0;
		__sync_synchronize();
	} else {
		result = hid_channel_handler(connection, channel, 0);
		if (result >= 0 && (channel_flags & 2U) && !(channel_flags & 1U))
			result = l2cap_connect(connection, channel);
	}
	active_hid_start_call = 0;
	__sync_synchronize();
	active_hid_start_connection = NULL;
	active_hid_start_channel = NULL;
	if (result < 0) {
		switch_connection = previous_connection;
		if (psm == L2CAP_PSM_HID_CONTROL)
			hid_control_channel = previous_channel;
		else
			hid_interrupt_channel = previous_channel;
	} else if (use_deferred_channel) {
		deferred_hid_connection = NULL;
		deferred_hid_control_channel = NULL;
	}
	publish_active_reconnect(psm == L2CAP_PSM_HID_CONTROL ? 2 : 4,
		psm, result,
		local_cid);
	return result;
}

static void kick_active_control(void)
{
	if (!__sync_bool_compare_and_swap(&active_reconnect_state,
	    ACTIVE_RECONNECT_CONTROL_START, ACTIVE_RECONNECT_CONTROL_PENDING))
		return;
	active_reconnect_started = ksceKernelGetSystemTimeLow();
	if (start_active_hid_channel(L2CAP_PSM_HID_CONTROL) >= 0) {
		procon_init(&procon_state, controller_mac);
		procon_timer = 0;
		return;
	}
	/* A missing/invalid SceBt connection object cannot recover by retrying the
	 * same lookup every 5 ms.  The old loop also reset its timeout on every
	 * attempt, flooding the trace indefinitely and leaving NEW PAIR racing a
	 * half-open ACL.  Tear the failed attempt down before another input retry. */
	if (__sync_bool_compare_and_swap(&active_reconnect_state,
	    ACTIVE_RECONNECT_CONTROL_PENDING, ACTIVE_RECONNECT_DISCONNECTING))
		active_reconnect_started = 0;
}

static void handle_active_reconnect(void)
{
	uint32_t now = ksceKernelGetSystemTimeLow();
	uint32_t age = now - active_reconnect_started;
	switch (active_reconnect_state) {
	case ACTIVE_RECONNECT_IDLE:
		return;
	case ACTIVE_RECONNECT_PASSIVE_WAIT:
		if (switch_acl_handle) {
			active_reconnect_state = ACTIVE_RECONNECT_PASSIVE_ACL_WAIT;
			active_reconnect_started = now;
			return;
		}
		if (age >= RECONNECT_PASSIVE_WAIT_US) {
			uint32_t address_low;
			uint16_t address_high;
			if (!bond_state_read_peer(&address_low, &address_high)) {
				/* With no learned peer there is nothing safe to page.  Keep the
				 * already-enabled passive scan as a normal first-pairing window. */
				active_reconnect_scan_active = 0;
				pairing_discoverable_active = 1;
				pairing_discoverable_started = now;
				active_reconnect_state = ACTIVE_RECONNECT_IDLE;
				active_reconnect_attempts = 0;
				publish_active_reconnect(0x0c, 0, 0, 0);
				return;
			}
			active_reconnect_context = NULL;
			int result = connect_switch_peer();
			active_reconnect_attempts++;
			active_reconnect_started = now;
			active_reconnect_state = result >= 0 ?
				ACTIVE_RECONNECT_ACL_PENDING : ACTIVE_RECONNECT_RETRY_WAIT;
		}
		return;
	case ACTIVE_RECONNECT_PASSIVE_ACL_WAIT:
		if (age >= RECONNECT_ACL_TIMEOUT_US)
			request_active_disconnect(10);
		return;
	case ACTIVE_RECONNECT_ACL_PENDING:
		if (age >= RECONNECT_ACL_TIMEOUT_US)
			request_active_disconnect(1);
		return;
	case ACTIVE_RECONNECT_AUTH_START:
		if (age < RECONNECT_AUTH_START_DELAY_US)
			return;
		{
			if (!__sync_bool_compare_and_swap(&active_reconnect_state,
			    ACTIVE_RECONNECT_AUTH_START,
			    ACTIVE_RECONNECT_AUTH_PENDING))
				return;
			active_reconnect_started = now;
			uint16_t handle = switch_acl_handle;
			int result = handle && hci_command ?
				hci_command(hci_context,
					HCI_OPCODE_AUTHENTICATION_REQUESTED, "2",
					handle) : -1;
			publish_active_reconnect(0x0e,
				HCI_OPCODE_AUTHENTICATION_REQUESTED, result, handle);
			if (result < 0 && __sync_bool_compare_and_swap(
			    &active_reconnect_state, ACTIVE_RECONNECT_AUTH_PENDING,
			    ACTIVE_RECONNECT_DISCONNECTING)) {
				active_reconnect_started = 0;
				request_active_disconnect(0x11);
			}
		}
		return;
	case ACTIVE_RECONNECT_AUTH_PENDING:
		if (age >= RECONNECT_ACL_TIMEOUT_US)
			request_active_disconnect(0x12);
		return;
	case ACTIVE_RECONNECT_ENCRYPT_PENDING:
		if (age >= RECONNECT_ACL_TIMEOUT_US)
			request_active_disconnect(0x14);
		return;
	case ACTIVE_RECONNECT_CONTROL_START:
		if (age < RECONNECT_HID_START_DELAY_US)
			return;
		if (raw_acl_reconnect) {
			if (!__sync_bool_compare_and_swap(&active_reconnect_state,
			    ACTIVE_RECONNECT_CONTROL_START,
			    ACTIVE_RECONNECT_CONTROL_PENDING))
				return;
			active_reconnect_started = now;
			int result = send_raw_control_probe();
			if (result < 0 && __sync_bool_compare_and_swap(
			    &active_reconnect_state, ACTIVE_RECONNECT_CONTROL_PENDING,
			    ACTIVE_RECONNECT_DISCONNECTING))
				active_reconnect_started = 0;
			return;
		}
		kick_active_control();
		if (active_reconnect_state == ACTIVE_RECONNECT_CONTROL_START &&
		    age >= RECONNECT_L2CAP_TIMEOUT_US) {
			request_active_disconnect(3);
		}
		return;
	case ACTIVE_RECONNECT_CONTROL_PENDING:
		if (age >= RECONNECT_L2CAP_TIMEOUT_US)
			request_active_disconnect(3);
		return;
	case ACTIVE_RECONNECT_CONTROL_CONFIG_START:
		if (!__sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_CONTROL_CONFIG_START,
		    ACTIVE_RECONNECT_CONTROL_CONFIG_PENDING))
			return;
		active_reconnect_started = now;
		if (send_raw_control_configuration() < 0 &&
		    __sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_CONTROL_CONFIG_PENDING,
		    ACTIVE_RECONNECT_DISCONNECTING))
			active_reconnect_started = 0;
		return;
	case ACTIVE_RECONNECT_CONTROL_CONFIG_PENDING:
		if (age >= RECONNECT_L2CAP_TIMEOUT_US)
			request_active_disconnect(0x15);
		return;
	case ACTIVE_RECONNECT_RAW_INTERRUPT_START:
		if (!__sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_RAW_INTERRUPT_START,
		    ACTIVE_RECONNECT_RAW_INTERRUPT_PENDING))
			return;
		active_reconnect_started = now;
		if (send_raw_interrupt_probe() < 0 &&
		    __sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_RAW_INTERRUPT_PENDING,
		    ACTIVE_RECONNECT_DISCONNECTING))
			active_reconnect_started = 0;
		return;
	case ACTIVE_RECONNECT_RAW_INTERRUPT_PENDING:
		if (age >= RECONNECT_L2CAP_TIMEOUT_US)
			request_active_disconnect(0x16);
		return;
	case ACTIVE_RECONNECT_RAW_INTERRUPT_CONFIG_START:
		if (!__sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_RAW_INTERRUPT_CONFIG_START,
		    ACTIVE_RECONNECT_RAW_INTERRUPT_CONFIG_PENDING))
			return;
		active_reconnect_started = now;
		if (send_raw_interrupt_configuration() < 0 &&
		    __sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_RAW_INTERRUPT_CONFIG_PENDING,
		    ACTIVE_RECONNECT_DISCONNECTING))
			active_reconnect_started = 0;
		return;
	case ACTIVE_RECONNECT_RAW_INTERRUPT_CONFIG_PENDING:
		if (age >= RECONNECT_L2CAP_TIMEOUT_US)
			request_active_disconnect(0x17);
		return;
	case ACTIVE_RECONNECT_RAW_BOOTSTRAP_START:
		if (!__sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_RAW_BOOTSTRAP_START,
		    ACTIVE_RECONNECT_RAW_BOOTSTRAP_WAIT))
			return;
		active_reconnect_started = now;
		if (send_raw_initial_input(0xde, 0) < 0 &&
		    __sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_RAW_BOOTSTRAP_WAIT,
		    ACTIVE_RECONNECT_DISCONNECTING))
			active_reconnect_started = 0;
		return;
	case ACTIVE_RECONNECT_RAW_BOOTSTRAP_WAIT:
		if (age >= RECONNECT_L2CAP_TIMEOUT_US)
			request_active_disconnect(0x18);
		return;
	case ACTIVE_RECONNECT_RAW_DEVICE_INFO_REPLY_START:
		if (!__sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_RAW_DEVICE_INFO_REPLY_START,
		    ACTIVE_RECONNECT_RAW_DEVICE_INFO_REPLY_WAIT))
			return;
		active_reconnect_started = now;
		if (send_raw_subcommand_reply(0x02, 0xe0) < 0 &&
		    __sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_RAW_DEVICE_INFO_REPLY_WAIT,
		    ACTIVE_RECONNECT_DISCONNECTING))
			active_reconnect_started = 0;
		return;
	case ACTIVE_RECONNECT_RAW_DEVICE_INFO_REPLY_WAIT:
		if (age >= RECONNECT_L2CAP_TIMEOUT_US)
			request_active_disconnect(0x19);
		return;
	case ACTIVE_RECONNECT_RAW_COMMAND08_REPLY_START:
		if (!__sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_RAW_COMMAND08_REPLY_START,
		    ACTIVE_RECONNECT_RAW_COMMAND08_REPLY_WAIT))
			return;
		active_reconnect_started = now;
		if (send_raw_subcommand_reply(0x08, 0xe2) < 0 &&
		    __sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_RAW_COMMAND08_REPLY_WAIT,
		    ACTIVE_RECONNECT_DISCONNECTING))
			active_reconnect_started = 0;
		return;
	case ACTIVE_RECONNECT_RAW_COMMAND08_REPLY_WAIT:
		if (age >= RECONNECT_L2CAP_TIMEOUT_US)
			request_active_disconnect(0x1a);
		return;
	case ACTIVE_RECONNECT_RAW_COMMAND10_REPLY_START:
		if (!__sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_RAW_COMMAND10_REPLY_START,
		    ACTIVE_RECONNECT_RAW_COMMAND10_REPLY_WAIT))
			return;
		active_reconnect_started = now;
		if (send_raw_subcommand_reply(0x10, 0xe4) < 0 &&
		    __sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_RAW_COMMAND10_REPLY_WAIT,
		    ACTIVE_RECONNECT_DISCONNECTING))
			active_reconnect_started = 0;
		return;
	case ACTIVE_RECONNECT_RAW_COMMAND10_REPLY_WAIT:
		if (age >= RECONNECT_L2CAP_TIMEOUT_US)
			request_active_disconnect(0x1b);
		return;
	case ACTIVE_RECONNECT_RAW_SPI2_REPLY_START:
		if (!__sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_RAW_SPI2_REPLY_START,
		    ACTIVE_RECONNECT_RAW_SPI2_REPLY_WAIT))
			return;
		active_reconnect_started = now;
		if (send_raw_subcommand_reply(0x10, 0xe6) < 0 &&
		    __sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_RAW_SPI2_REPLY_WAIT,
		    ACTIVE_RECONNECT_DISCONNECTING))
			active_reconnect_started = 0;
		return;
	case ACTIVE_RECONNECT_RAW_SPI2_REPLY_WAIT:
		if (age >= RECONNECT_L2CAP_TIMEOUT_US)
			request_active_disconnect(0x1c);
		return;
	case ACTIVE_RECONNECT_RAW_INIT_SCRIPT_REPLY_START: {
		if (!__sync_bool_compare_and_swap(&active_reconnect_state,
		    ACTIVE_RECONNECT_RAW_INIT_SCRIPT_REPLY_START,
		    ACTIVE_RECONNECT_RAW_INIT_SCRIPT_REPLY_WAIT))
			return;
		active_reconnect_started = now;
		unsigned int script_stage = raw_init_script_stage;
		unsigned int script_count = ARRAY_SIZE(raw_init_script);
		int terminal_stage = script_stage == script_count - 1U;
		if (terminal_stage)
			input_send_ready = 0;
		int result = script_stage < script_count ?
			send_raw_subcommand_reply(
				raw_init_script[script_stage].command, 0xe8) : -1;
		/* The clean native trace is already streaming A1 30 reports by the
		 * final calibration read.  Keep this diagnostic bounded: enqueue one
		 * neutral transition prompt after the stage-6 reply, but do not start a
		 * cadence or alter any earlier scripted exchange. */
		if (result >= 0 &&
		    script_stage == RAW_INIT_VALID_TRANSITION_STAGE)
			result = send_raw_initial_input(RAW_TRACE_VALID_INPUT,
				procon_timer++);
		if (result >= 0 && terminal_stage) {
			if (__sync_bool_compare_and_swap(&active_reconnect_state,
			    ACTIVE_RECONNECT_RAW_INIT_SCRIPT_REPLY_WAIT,
			    ACTIVE_RECONNECT_IDLE)) {
				raw_subcommand_request_length = 0;
				raw_input_pending = 0;
				raw_input_active = 1;
				subcommands_started = 1;
				continuous_input_enabled = 1;
				pairing_confirmed = 1;
				keepalive_active = 1;
				last_input_report_time = 0;
				raw_connected_input_reports = 0;
				active_reconnect_attempts = 0;
				active_reconnect_started = 0;
				__sync_synchronize();
				publish_active_reconnect(RAW_TRACE_ESTABLISHED,
					procon_state.player_lights, result,
					raw_interrupt_remote_cid);
				return;
			}
			result = -3;
		}
		if (result < 0) {
			if (__sync_bool_compare_and_swap(&active_reconnect_state,
			    ACTIVE_RECONNECT_RAW_INIT_SCRIPT_REPLY_WAIT,
			    ACTIVE_RECONNECT_DISCONNECTING))
				active_reconnect_started = 0;
		}
		return;
	}
	case ACTIVE_RECONNECT_RAW_INIT_SCRIPT_REPLY_WAIT:
		if (age >= RECONNECT_L2CAP_TIMEOUT_US)
			request_active_disconnect(0x1d);
		return;
	case ACTIVE_RECONNECT_INTERRUPT_START:
		/* SceBt's configured control callback starts PSM 0x13.  Its successful
		 * Connection Response is adopted by local CID in acl_event_hook. */
		if (age >= RECONNECT_L2CAP_TIMEOUT_US)
			request_active_disconnect(5);
		return;
	case ACTIVE_RECONNECT_INTERRUPT_PENDING:
		if (age >= RECONNECT_L2CAP_TIMEOUT_US)
			request_active_disconnect(5);
		return;
	case ACTIVE_RECONNECT_DISCONNECTING:
		if (!active_reconnect_started) {
			request_active_disconnect(6);
			return;
		}
		if (!switch_acl_handle) {
			raw_acl_reconnect = 0;
			reset_raw_control_channel();
			active_reconnect_state = ACTIVE_RECONNECT_RETRY_WAIT;
			active_reconnect_started = now;
		} else if (age >= RECONNECT_L2CAP_TIMEOUT_US) {
			/* The controller may already have dropped the dead link without
			 * SceBt delivering Disconnect Complete to our worker.  Clear only
			 * plugin-owned references.  If the native ACL is actually still live,
			 * the controller will reject a duplicate Create Connection command. */
			switch_acl_handle = 0;
			switch_connection = NULL;
			hid_control_channel = NULL;
			hid_interrupt_channel = NULL;
			deferred_hid_connection = NULL;
			deferred_hid_control_channel = NULL;
			active_reconnect_context = NULL;
			raw_acl_reconnect = 0;
			reset_raw_control_channel();
			active_reconnect_state = ACTIVE_RECONNECT_RETRY_WAIT;
			active_reconnect_started = now;
		}
		return;
	case ACTIVE_RECONNECT_RETRY_WAIT:
		if (age < RECONNECT_RETRY_DELAY_US)
			return;
		if (active_reconnect_attempts < 2 && controller_app_active) {
			active_reconnect_state = ACTIVE_RECONNECT_IDLE;
			reconnect_requested = 1;
			publish_active_reconnect(7, 0, 0, 0);
		} else {
			/* Never turn a failed bonded reconnect into a fresh registration.
			 * NEW PAIR is the explicit path into inquiry discoverability. */
			int bonded = bond_state_has_saved_bond();
			int result = bonded ? set_scan_and_wait(0) :
				force_discoverable_scan();
			if (!bonded && result >= 0) {
				active_reconnect_scan_active = 0;
				pairing_discoverable_active = 1;
				pairing_discoverable_started = now;
			}
			active_reconnect_state = ACTIVE_RECONNECT_IDLE;
			active_reconnect_attempts = 0;
			raw_acl_reconnect = 0;
			reset_raw_control_channel();
			publish_active_reconnect(8, 0, result, 0);
		}
		return;
	default:
		active_reconnect_state = ACTIVE_RECONNECT_IDLE;
		raw_acl_reconnect = 0;
		reset_raw_control_channel();
		return;
	}
}

static void publish_pairing_reset(uint8_t phase, int result)
{
	uint8_t probe[8] = { 0x50, phase, (uint8_t)result,
		(uint8_t)(result >> 8), (uint8_t)(result >> 16),
		(uint8_t)(result >> 24), (uint8_t)switch_acl_handle,
		(uint8_t)(switch_acl_handle >> 8) };
	publish(10, probe, sizeof(probe));
}

static int set_page_scan_only(void)
{
	/* HCI Write Scan Enable: page scan only, not inquiry-discoverable. */
	return hci_command(hci_context, HCI_OPCODE_WRITE_SCAN_ENABLE, "1", 2);
}

static int wait_for_counter(volatile unsigned int *counter,
	unsigned int expected)
{
	uint32_t started = ksceKernelGetSystemTimeLow();
	while (*counter < expected) {
		if (ksceKernelGetSystemTimeLow() - started >= SCAN_COMMAND_TIMEOUT_US)
			return -2;
		ksceKernelDelayThread(5000);
	}
	return 0;
}

static int set_scan_and_wait(int discoverable)
{
	unsigned int issued_before = scan_write_issued;
	int result = set_scan ? set_scan(discoverable) : -1;
	if (result < 0)
		return result;
	unsigned int expected = scan_write_issued;
	/* The wrapper emits no HCI command when its cached state already matches. */
	if (expected == issued_before)
		return 0;
	result = wait_for_counter(&scan_write_completed, expected);
	if (result < 0)
		return result;
	return scan_write_status ? -scan_write_status : 0;
}

static int read_scan_enable(uint8_t *value)
{
	unsigned int issued_before = scan_read_issued;
	int result = hci_command ?
		hci_command(hci_context, HCI_OPCODE_READ_SCAN_ENABLE, "") : -1;
	if (result < 0)
		return result;
	unsigned int expected = scan_read_issued;
	if (expected == issued_before)
		return -3;
	result = wait_for_counter(&scan_read_completed, expected);
	if (result < 0)
		return result;
	if (scan_read_status)
		return -scan_read_status;
	*value = scan_read_value;
	return 0;
}

static int force_discoverable_scan(void)
{
	if (!set_scan || !hci_command || !controller_app_active)
		return -1;
	/* This runs only in the worker after a foreground-app request.  The first
	 * transition repairs SceBt's cached boolean; waiting for Command Complete
	 * prevents the second transition from racing it. */
	for (int attempt = 0; attempt < 3; attempt++) {
		int result = set_scan_and_wait(0);
		if (result < 0)
			continue;
		result = set_scan_and_wait(1);
		if (result < 0)
			continue;
		uint8_t value = 0;
		result = read_scan_enable(&value);
		if (result >= 0 && value == 3)
			return 0;
	}
	return -4;
}

static void handle_pairing_reset(void)
{
	if (!hci_command || !set_scan)
		return;
	if (pairing_reset_requested) {
		pairing_reset_requested = 0;
		pairing_resume_requested = 0;
		reconnect_requested = 0;
		active_reconnect_scan_active = 0;
		active_reconnect_state = ACTIVE_RECONNECT_IDLE;
		active_reconnect_attempts = 0;
		if (switch_acl_handle) {
			uint16_t handle = switch_acl_handle;
			keepalive_active = 0;
			force_disconnect = 1;
			int result = hci_command(hci_context, HCI_OPCODE_DISCONNECT,
				"21", handle,
				HCI_REASON_REMOTE_USER_TERMINATED_CONNECTION);
			force_disconnect = 0;
			pairing_reset_state = 1;
			pairing_reset_started = ksceKernelGetSystemTimeLow();
			publish_pairing_reset(1, result);
			return;
		}
		pairing_reset_state = 2;
	}
	if (pairing_reset_state == 1) {
		uint32_t age = ksceKernelGetSystemTimeLow() - pairing_reset_started;
		if (switch_acl_handle && age < 2000000U)
			return;
		pairing_reset_state = 2;
	}
	if (pairing_reset_state == 2) {
		/* A Vita-only key deletion leaves the Switch holding a stale key and
		 * makes re-pairing less reliable.  NEW PAIR restarts the passive radio
		 * flow while retaining a valid bond; explicit `forget` remains available
		 * for tests where the Switch-side record is also removed. */
		int scan_result = force_discoverable_scan();
		if (scan_result >= 0) {
			pairing_discoverable_active = 1;
			pairing_discoverable_started = ksceKernelGetSystemTimeLow();
		}
		pairing_reset_state = 0;
		publish_pairing_reset(2, scan_result);
	}
	if (pairing_resume_requested && !pairing_reset_state) {
		pairing_resume_requested = 0;
		reconnect_requested = 0;
		int scan_result = force_discoverable_scan();
		if (scan_result >= 0) {
			pairing_discoverable_active = 1;
			pairing_discoverable_started = ksceKernelGetSystemTimeLow();
		}
		/* Phase 4: disconnected L+R opened a pairing window without
		 * deleting the existing Switch link key. */
		publish_pairing_reset(4, scan_result);
	}
}

static void update_pairing_discoverable(void)
{
	if (active_reconnect_scan_active) {
		uint32_t reconnect_age = ksceKernelGetSystemTimeLow() -
			active_reconnect_scan_started;
		if (!controller_app_active || switch_acl_handle ||
		    reconnect_age >= PAIRING_DISCOVERABLE_TIMEOUT_US) {
			int reconnect_result = set_scan_and_wait(0);
			if (reconnect_result >= 0)
				active_reconnect_scan_active = 0;
			uint8_t scan_probe[4] = { 0x56, 2,
				(uint8_t)reconnect_result,
				(uint8_t)(reconnect_result >> 8) };
			publish(10, scan_probe, sizeof(scan_probe));
		}
	}
	if (!pairing_discoverable_active)
		return;
	uint32_t age = ksceKernelGetSystemTimeLow() - pairing_discoverable_started;
	if (controller_app_active && !switch_acl_handle &&
	    age < PAIRING_DISCOVERABLE_TIMEOUT_US)
		return;
	int result = set_scan_and_wait(0);
	if (result >= 0)
		pairing_discoverable_active = 0;
	publish_pairing_reset(3, result);
}

static void append_line(const char *line, int length)
{
	SceUID fd = ksceIoOpen(TRACE_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd >= 0) {
		ksceIoWrite(fd, line, length);
		ksceIoClose(fd);
	}
}

static int bounded_line_length(int length, size_t capacity)
{
	if (length <= 0 || capacity == 0)
		return 0;
	if ((size_t)length >= capacity)
		return (int)capacity - 1;
	return length;
}

static void publish_peer_storage(uint8_t phase, int result,
	uint32_t low, uint16_t high)
{
	uint8_t probe[10] = { 0x57, phase, (uint8_t)result,
		(uint8_t)(result >> 8), (uint8_t)low, (uint8_t)(low >> 8),
		(uint8_t)(low >> 16), (uint8_t)(low >> 24),
		(uint8_t)high, (uint8_t)(high >> 8) };
	publish(10, probe, sizeof(probe));
}

static int load_peer_address(void)
{
	uint8_t record[PEER_RECORD_SIZE];
	SceUID fd = ksceIoOpen(PEER_PATH, SCE_O_RDONLY, 0);
	if (fd < 0)
		return fd;
	int length = ksceIoRead(fd, record, sizeof(record));
	ksceIoClose(fd);
	if (length != (int)sizeof(record))
		return length < 0 ? length : -1;
	uint32_t low;
	uint16_t high;
	int result = vita_ns_decode_peer_record(record, &low, &high);
	if (result < 0)
		return result;
	bond_state_set_peer(low, high);
	publish_peer_storage(1, 0, low, high);
	return 0;
}

static void handle_peer_address_save(void)
{
	if (!switch_address_save_pending)
		return;
	uint32_t now = ksceKernelGetSystemTimeLow();
	if (switch_address_save_attempted &&
	    now - switch_address_save_attempted < PEER_SAVE_RETRY_US)
		return;
	switch_address_save_attempted = now;
	uint32_t low;
	uint16_t high;
	if (!bond_state_read_peer(&low, &high)) {
		switch_address_save_pending = 0;
		return;
	}
	uint8_t record[PEER_RECORD_SIZE];
	vita_ns_encode_peer_record(record, low, high);
	SceUID fd = ksceIoOpen(PEER_PATH,
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	int result = fd;
	if (fd >= 0) {
		result = ksceIoWrite(fd, record, sizeof(record));
		int close_result = ksceIoClose(fd);
		if (result == (int)sizeof(record))
			result = close_result < 0 ? close_result : 0;
		else if (result >= 0)
			result = -1;
	}
	if (result >= 0 && bond_state_peer_matches(low, high))
		switch_address_save_pending = 0;
	publish_peer_storage(2, result, low, high);
}

static void publish_link_key_storage(uint8_t phase, int result,
	uint32_t low, uint16_t high, uint8_t key_type)
{
	uint8_t probe[10] = { 0x5A, phase, key_type,
		(uint8_t)result, (uint8_t)(result >> 8),
		(uint8_t)(result >> 16), (uint8_t)(result >> 24),
		(uint8_t)low, (uint8_t)high, (uint8_t)(high >> 8) };
	publish(10, probe, sizeof(probe));
}

static int load_link_key(void)
{
	uint8_t record[LINK_KEY_RECORD_SIZE];
	SceUID fd = ksceIoOpen(LINK_KEY_PATH, SCE_O_RDONLY, 0);
	if (fd < 0)
		return fd;
	int length = ksceIoRead(fd, record, sizeof(record));
	ksceIoClose(fd);
	if (length != (int)sizeof(record)) {
		memset(record, 0, sizeof(record));
		return length < 0 ? length : -1;
	}
	uint32_t low;
	uint16_t high;
	uint8_t key[16];
	uint8_t key_type;
	int result = vita_ns_decode_link_key_record(record, &low, &high, key,
		&key_type);
	memset(record, 0, sizeof(record));
	if (result < 0)
		return result;
	uint32_t peer_low;
	uint16_t peer_high;
	if (bond_state_read_peer(&peer_low, &peer_high)) {
		if (peer_low != low || peer_high != high) {
			memset(key, 0, sizeof(key));
			return -3;
		}
	} else {
		bond_state_set_peer(low, high);
		switch_address_save_pending = 1;
	}
	bond_state_set_link_key(low, high, key, key_type);
	memset(key, 0, sizeof(key));
	publish_link_key_storage(2, 0, low, high, key_type);
	return 0;
}

static void handle_pending_link_key(void)
{
	uint32_t low;
	uint16_t high;
	uint8_t key[16];
	uint8_t key_type;
	unsigned int generation;
	if (!bond_state_read_pending_link_key(&low, &high, key, &key_type,
	    &generation))
		return;
	if (generation == pending_link_key_consumed) {
		memset(key, 0, sizeof(key));
		return;
	}
	bond_state_set_link_key(low, high, key, key_type);
	pending_link_key_consumed = generation;
	switch_link_key_save_pending = 1;
	switch_link_key_save_attempted = 0;
	publish_link_key_storage(5, 0, low, high, key_type);
	memset(key, 0, sizeof(key));
}

static void handle_link_key_save(void)
{
	if (!switch_link_key_save_pending)
		return;
	uint32_t now = ksceKernelGetSystemTimeLow();
	if (switch_link_key_save_attempted &&
	    now - switch_link_key_save_attempted < PEER_SAVE_RETRY_US)
		return;
	switch_link_key_save_attempted = now;
	uint32_t low;
	uint16_t high;
	uint8_t key[16];
	uint8_t key_type;
	if (!bond_state_read_link_key(&low, &high, key, &key_type)) {
		switch_link_key_save_pending = 0;
		return;
	}
	uint8_t record[LINK_KEY_RECORD_SIZE];
	vita_ns_encode_link_key_record(record, low, high, key, key_type);
	SceUID fd = ksceIoOpen(LINK_KEY_PATH,
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	int result = fd;
	if (fd >= 0) {
		result = ksceIoWrite(fd, record, sizeof(record));
		int close_result = ksceIoClose(fd);
		if (result == (int)sizeof(record))
			result = close_result < 0 ? close_result : 0;
		else if (result >= 0)
			result = -1;
	}
	memset(record, 0, sizeof(record));
	if (result >= 0) {
		uint32_t current_low;
		uint16_t current_high;
		uint8_t current_key[16];
		uint8_t current_type;
		if (bond_state_read_link_key(&current_low, &current_high,
		    current_key, &current_type) && current_low == low &&
		    current_high == high && current_type == key_type &&
		    !memcmp(current_key, key, sizeof(key)))
			switch_link_key_save_pending = 0;
		memset(current_key, 0, sizeof(current_key));
	}
	publish_link_key_storage(3, result, low, high, key_type);
	memset(key, 0, sizeof(key));
}

static void apply_identity(int *name_result, int *eir_result, int *class_result,
	int *scan_result, int *scan_attempts)
{
	*name_result = hci_command(hci_context, 0x0C13, hci_name_format, hci_local_name);
	ksceKernelDelayThread(100000);
	*eir_result = hci_command(hci_context, 0x0C52,
		hci_eir_format, 1, procon_eir, sizeof(procon_eir));
	ksceKernelDelayThread(100000);
	*scan_result = -1;
	*scan_attempts = 0;
	while (*scan_attempts < 100) {
		(*scan_attempts)++;
		*scan_result = set_page_scan_only();
		if (*scan_result >= 0)
			break;
		ksceKernelDelayThread(100000);
	}
	ksceKernelDelayThread(100000);
	*class_result = hci_command(hci_context, 0x0C24, hci_class_format, 0x002508);
}

static void handle_command(void)
{
	char command[16];
	SceUID fd = ksceIoOpen(COMMAND_PATH, SCE_O_RDONLY, 0);
	if (fd < 0)
		return;
	int length = ksceIoRead(fd, command, sizeof(command));
	ksceIoClose(fd);
	ksceIoRemove(COMMAND_PATH);
	if (length >= 5 && !memcmp(command, "apply", 5)) {
		int name_result, eir_result, class_result, scan_result, scan_attempts;
		char line[128];
		apply_identity(&name_result, &eir_result, &class_result,
			&scan_result, &scan_attempts);
		int n = snprintf(line, sizeof(line),
			"reapply name=0x%08X eir=0x%08X scan=0x%08X class=0x%08X tries=%d\n",
			name_result, eir_result, scan_result, class_result, scan_attempts);
		append_line(line, bounded_line_length(n, sizeof(line)));
	} else if (length >= 5 && !memcmp(command, "clear", 5)) {
		fd = ksceIoOpen(TRACE_PATH, SCE_O_WRONLY | SCE_O_TRUNC, 0);
		if (fd >= 0)
			ksceIoClose(fd);
		append_line("trace_cleared\n", 14);
	} else if (length >= 6 && !memcmp(command, "forget", 6)) {
		/* HCI transports BD_ADDR least-significant byte first.  The `6`
		 * serializer item is passed as an ABI-aligned uint64_t; only its low
		 * six bytes are emitted.  Delete_All_Flag=0 limits this to the Switch. */
		uint32_t address_low = 0;
		uint16_t address_high = 0;
		int have_peer = bond_state_read_peer(&address_low, &address_high);
		const uint64_t switch_address = have_peer ? address_low |
			((uint64_t)address_high << 32) : 0;
		int result = have_peer ? hci_command(hci_context, 0x0C12,
			hci_delete_key_format, switch_address, 0) : -1;
		bond_state_clear_link_key();
		switch_link_key_save_pending = 0;
		switch_link_key_save_attempted = 0;
		int file_result = ksceIoRemove(LINK_KEY_PATH);
		publish_link_key_storage(4, file_result, address_low, address_high, 0);
		char line[160];
		int n = snprintf(line, sizeof(line),
			"forget_switch result=0x%08X key_file=0x%08X "
			"address=%02X:%02X:%02X:%02X:%02X:%02X\n",
			result, file_result, (uint8_t)(address_high >> 8), (uint8_t)address_high,
			(uint8_t)(address_low >> 24), (uint8_t)(address_low >> 16),
			(uint8_t)(address_low >> 8), (uint8_t)address_low);
		append_line(line, bounded_line_length(n, sizeof(line)));
	}
}

static int trace_worker(SceSize argc, void *args)
{
	(void)argc;
	(void)args;
	unsigned int read_index = 0;
	char line[1024];

	/* Loading the kernel plugin must not modify SceBt or start high-frequency
	 * helpers while the shell, lock screen, Wi-Fi and Bluetooth services are
	 * still starting.  The controller app's first input submission opens this
	 * gate.  A low-frequency wait is the plugin's only boot-time activity. */
	while (!stop_requested && !controller_app_active)
		ksceKernelDelayThread(100000);
	if (stop_requested)
		return 0;

	SceUID fd = ksceIoOpen(TRACE_PATH,
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd >= 0)
		ksceIoClose(fd);
	int peer_load_result = load_peer_address();
	int link_key_load_result = load_link_key();
	uint32_t peer_low = 0;
	uint16_t peer_high = 0;
	int peer_valid = bond_state_read_peer(&peer_low, &peer_high);
	uint8_t loaded_key[16];
	uint8_t loaded_key_type = 0;
	uint32_t loaded_key_low = 0;
	uint16_t loaded_key_high = 0;
	int link_key_valid = bond_state_read_link_key(&loaded_key_low,
		&loaded_key_high, loaded_key, &loaded_key_type);
	memset(loaded_key, 0, sizeof(loaded_key));
	int peer_line_length = snprintf(line, sizeof(line),
		"peer_load=0x%08X valid=%d address=%02X:%02X:%02X:%02X:%02X:%02X "
		"link_key_load=0x%08X valid=%d type=%u\n",
		peer_load_result, peer_valid, (uint8_t)(peer_high >> 8),
		(uint8_t)peer_high, (uint8_t)(peer_low >> 24),
		(uint8_t)(peer_low >> 16), (uint8_t)(peer_low >> 8),
		(uint8_t)peer_low, link_key_load_result, link_key_valid,
		loaded_key_type);
	append_line(line, bounded_line_length(peer_line_length, sizeof(line)));
	proc_event_uid = ksceKernelRegisterProcEventHandler("vita_ns_pad_gate",
		&controller_proc_handler, 0);
	sender_uid = ksceKernelCreateThread("scebt_input_sender", sender_worker,
		0x10000110, 0x4000, 0, 0, NULL);
	if (sender_uid < 0) {
		append_line("sender_create_failed\n", 21);
		if (proc_event_uid >= 0) {
			ksceKernelUnregisterProcEventHandler(proc_event_uid);
			proc_event_uid = -1;
		}
		return -1;
	}
	int sender_start_result = ksceKernelStartThread(sender_uid, 0, NULL);
	if (sender_start_result < 0) {
		ksceKernelDeleteThread(sender_uid);
		sender_uid = -1;
		if (proc_event_uid >= 0) {
			ksceKernelUnregisterProcEventHandler(proc_event_uid);
			proc_event_uid = -1;
		}
		int n = snprintf(line, sizeof(line),
			"sender_start_failed=0x%08X\n", sender_start_result);
		append_line(line, bounded_line_length(n, sizeof(line)));
		return -1;
	}

	for (int attempt = 0; attempt < 6000 && !stop_requested; attempt++) {
		tai_module_info_t module;
		memset(&module, 0, sizeof(module));
		module.size = sizeof(module);
		if (taiGetModuleInfoForKernel(KERNEL_PID, "SceBt", &module) >= 0) {
			if (module.module_nid != SCEBT_NID) {
				int n = snprintf(line, sizeof(line), "wrong_nid=0x%08X\n", module.module_nid);
				append_line(line, bounded_line_length(n, sizeof(line)));
				return -1;
			}
			SceKernelModuleInfo kernel_module;
			memset(&kernel_module, 0, sizeof(kernel_module));
			kernel_module.size = sizeof(kernel_module);
			if (ksceKernelGetModuleInfo(KERNEL_PID, module.modid, &kernel_module) < 0)
				return -1;
			uint8_t *base = kernel_module.segments[0].vaddr;
			uint8_t *data_base = kernel_module.segments[1].vaddr;
			if (!data_base || kernel_module.segments[1].memsz < 0x10) {
				append_line("missing_data_segment\n", 21);
				return -1;
			}
			static const uint8_t original_name[17] = "PlayStation Vita";
			static const uint8_t local_name[17] = "Pro Controller";
			static const uint8_t original_movw[4] = {0x43, 0xF6, 0x01, 0x63};
			static const uint8_t class_movw[4] = {0x42, 0xF2, 0x08, 0x53};
			static const uint8_t original_shift[2] = {0x1B, 0x02};
			static const uint8_t nop[2] = {0x00, 0xBF};
			static const uint8_t original_io_capability[2] = {0x01, 0x21};
			static const uint8_t no_input_output[2] = {0x03, 0x21};
			static const uint8_t original_auth_requirements[2] = {0x05, 0x23};
			/* Keep SceBt's normal general-bonding policy while disabling only
			 * MITM.  NoInputNoOutput cannot satisfy MITM, but disabling bonding
			 * also makes the first-pairing link key unusable for reconnect. */
			static const uint8_t no_mitm_general_bonding[2] = {0x04, 0x23};
			static const uint8_t original_scan_mask[4] = {0x43, 0xF0, 0x01, 0x05};
			static const uint8_t discoverable_scan_mask[4] = {0x43, 0xF0, 0x03, 0x05};
			static const char create_connection_format[] = "621-21";
			static const char address_format[] = "6";
			static const char link_key_reply_format[] = "6#";
			static const uint8_t hci_transport_enqueue_prologue[8] = {
				0x2d,0xe9,0xf0,0x47,0x4a,0xf2,0x04,0x07,
			};
			static const uint8_t original_record_sizes[5] = {0x32, 0x33, 0x33, 0x3c, 0x4e};
			static const uint16_t original_record_offsets[5] = {0, 0x32, 0x65, 0x98, 0xd4};
			static const uint8_t original_pnp_ids[24] = {
				0x06,0x02,0x00,0x09,0x01,0x03, 0x06,0x02,0x01,0x09,0x05,0x4c,
				0x06,0x02,0x02,0x09,0x05,0x8c, 0x06,0x02,0x03,0x09,0x01,0x00,
			};
			static const uint8_t procon_pnp_ids[24] = {
				0x06,0x02,0x00,0x09,0x01,0x03, 0x06,0x02,0x01,0x09,0x05,0x7e,
				0x06,0x02,0x02,0x09,0x20,0x09, 0x06,0x02,0x03,0x09,0x00,0x01,
			};
			int records_valid = 1;
			for (unsigned int i = 0; i < 5; i++)
				records_valid &= base[SDP_DB_OFFSET + original_record_offsets[i]] ==
					original_record_sizes[i];
			records_valid &= valid_sdp_records(hid_sdp_records, sizeof(hid_sdp_records));
			if (memcmp(base + LOCAL_NAME_OFFSET, original_name, sizeof(original_name)) ||
			    memcmp(base + CLASS_MOVW_OFFSET, original_movw, sizeof(original_movw)) ||
			    memcmp(base + CLASS_SHIFT_OFFSET, original_shift, sizeof(original_shift)) ||
			    memcmp(base + IO_CAPABILITY_OFFSET, original_io_capability,
				    sizeof(original_io_capability)) ||
			    memcmp(base + AUTH_REQUIREMENTS_OFFSET, original_auth_requirements,
				    sizeof(original_auth_requirements)) ||
			    memcmp(base + SCAN_MASK_OFFSET, original_scan_mask, sizeof(original_scan_mask)) ||
			    memcmp(base + 0x1EF74, address_format, sizeof(address_format)) ||
			    memcmp(base + 0x1F01C, link_key_reply_format,
				    sizeof(link_key_reply_format)) ||
			    memcmp(base + HCI_CREATE_CONNECTION_FORMAT_OFFSET,
				    create_connection_format, sizeof(create_connection_format)) ||
			    memcmp(base + (HCI_TRANSPORT_ENQUEUE_OFFSET & ~1U),
				    hci_transport_enqueue_prologue,
				    sizeof(hci_transport_enqueue_prologue)) ||
			    memcmp(base + PNP_IDS_OFFSET, original_pnp_ids, sizeof(original_pnp_ids)) ||
			    !records_valid || base[SDP_DB_OFFSET + SDP_AUDIO_SIZE] != 0x53) {
				append_line("patch_validation_failed\n", 24);
				return -1;
			}
			local_name_uid = taiInjectDataForKernel(KERNEL_PID, module.modid, 0,
				LOCAL_NAME_OFFSET, local_name, sizeof(local_name));
			class_movw_uid = taiInjectDataForKernel(KERNEL_PID, module.modid, 0,
				CLASS_MOVW_OFFSET, class_movw, sizeof(class_movw));
			class_shift_uid = taiInjectDataForKernel(KERNEL_PID, module.modid, 0,
				CLASS_SHIFT_OFFSET, nop, sizeof(nop));
			io_capability_uid = taiInjectDataForKernel(KERNEL_PID, module.modid, 0,
				IO_CAPABILITY_OFFSET, no_input_output, sizeof(no_input_output));
			auth_requirements_uid = taiInjectDataForKernel(KERNEL_PID, module.modid, 0,
				AUTH_REQUIREMENTS_OFFSET, no_mitm_general_bonding,
				sizeof(no_mitm_general_bonding));
			scan_mask_uid = taiInjectDataForKernel(KERNEL_PID, module.modid, 0,
				SCAN_MASK_OFFSET, discoverable_scan_mask, sizeof(discoverable_scan_mask));
			sdp_db_uid = taiInjectDataForKernel(KERNEL_PID, module.modid, 0,
				SDP_DB_OFFSET, hid_sdp_records, sizeof(hid_sdp_records));
			pnp_ids_uid = taiInjectDataForKernel(KERNEL_PID, module.modid, 0,
				PNP_IDS_OFFSET, procon_pnp_ids, sizeof(procon_pnp_ids));
			hci_command = (HciCommand)(base + HCI_COMMAND_OFFSET);
			hci_transport_enqueue = (HciTransportEnqueue)
				(base + HCI_TRANSPORT_ENQUEUE_OFFSET);
			hci_context = data_base + 0x10;
			hci_name_format = (const char *)(base + 0x1EFA4);
			hci_class_format = (const char *)(base + 0x1EFA0);
			hci_eir_format = (const char *)(base + 0x1EFC4);
			hci_delete_key_format = (const char *)(base + 0x1EF70);
			hci_address_format = (const char *)(base + 0x1EF74);
			hci_link_key_reply_format = (const char *)(base + 0x1F01C);
			hci_create_connection_format = (const char *)
				(base + HCI_CREATE_CONNECTION_FORMAT_OFFSET);
			hci_local_name = (const char *)(base + LOCAL_NAME_OFFSET);
			set_scan = (int (*)(int))(base + INQUIRY_SCAN_OFFSET);
			confirm_user = (int (*)(uint32_t, uint32_t, int))(base + CONFIRM_USER_OFFSET);
			connect_remote = (ConnectRemote)(base + CONNECT_REMOTE_OFFSET);
			hid_channel_handler = (HidChannelHandler)
				(base + HID_CHANNEL_HANDLER_OFFSET);
			find_connection = (FindConnection)(base + FIND_CONNECTION_OFFSET);
			find_channel = (FindChannel)(base + FIND_CHANNEL_OFFSET);
			l2cap_connection_response = (L2capConnectionResponse)
				(base + L2CAP_CONNECTION_RESPONSE_OFFSET);
			l2cap_connect = (L2capConnect)(base + L2CAP_CONNECT_OFFSET);
			l2cap_send = (L2capSend)(base + L2CAP_SEND_OFFSET);
			l2cap_raw_format = (const char *)(base + L2CAP_RAW_FORMAT_OFFSET);
			procon_init(&procon_state, controller_mac);
			hci_event_uid = taiHookFunctionOffsetForKernel(KERNEL_PID, &hci_event_ref,
				module.modid, 0, HCI_EVENT_OFFSET, 1, hci_event_hook);
			acl_event_uid = taiHookFunctionOffsetForKernel(KERNEL_PID, &acl_event_ref,
				module.modid, 0, ACL_EVENT_OFFSET, 1, acl_event_hook);
			hid_channel_uid = taiHookFunctionOffsetForKernel(KERNEL_PID, &hid_channel_ref,
				module.modid, 0, HID_CHANNEL_HANDLER_OFFSET, 1, hid_channel_hook);
			disconnect_direct_uid = taiHookFunctionOffsetForKernel(KERNEL_PID,
				&disconnect_direct_ref, module.modid, 0, DISCONNECT_DIRECT_OFFSET,
				1, disconnect_direct_hook);
			disconnect_cleanup_uid = taiHookFunctionOffsetForKernel(KERNEL_PID,
				&disconnect_cleanup_ref, module.modid, 0, DISCONNECT_CLEANUP_OFFSET,
				1, disconnect_cleanup_hook);
			hid_disconnect_uid = taiHookFunctionOffsetForKernel(KERNEL_PID,
				&hid_disconnect_ref, module.modid, 0, HID_DISCONNECT_OFFSET,
				1, hid_disconnect_hook);
			hci_command_uid = taiHookFunctionOffsetForKernel(KERNEL_PID,
				&hci_command_ref, module.modid, 0, HCI_COMMAND_OFFSET,
				1, hci_command_hook);
			l2cap_connect_uid = taiHookFunctionOffsetForKernel(KERNEL_PID,
				&l2cap_connect_ref, module.modid, 0, L2CAP_CONNECT_OFFSET,
				1, l2cap_connect_hook);
			int name_result, eir_result, class_result, scan_result, scan_attempts;
			apply_identity(&name_result, &eir_result, &class_result,
				&scan_result, &scan_attempts);
			ksceKernelDelayThread(100000);
			int bd_addr_result = hci_command(hci_context, 0x1009, "");
			int n = snprintf(line, sizeof(line),
				"modid=0x%08X name_uid=%d class_uids=%d,%d sec_uids=%d,%d "
				"sdp_uids=%d,%d scan_uid=%d "
				"name=0x%08X eir=0x%08X scan=0x%08X class=0x%08X tries=%d "
					"packet_hooks=%d,%d hid_hook=%d disconnect_hooks=%d,%d,%d "
					"hci_hook=%d l2cap_connect_hook=%d bd_addr=0x%08X "
					"proc_event=%d motion=0 raw_cfg11=1 raw_cfg13=1 "
					"raw_bootstrap=1 raw_device_info=1 raw_command08=1 "
					"raw_command10=1 raw_spi2=1 raw_init_script=1 "
					"raw_transition=1 raw_valid_transition=1 trace_line_safe=1 "
					"raw_connected_input=1 input_gated_reconnect=1\n",
				module.modid, local_name_uid, class_movw_uid, class_shift_uid,
				io_capability_uid, auth_requirements_uid,
				sdp_db_uid, pnp_ids_uid, scan_mask_uid, name_result, eir_result,
				scan_result, class_result, scan_attempts,
				hci_event_uid, acl_event_uid, hid_channel_uid,
				disconnect_direct_uid, disconnect_cleanup_uid, hid_disconnect_uid,
				hci_command_uid, l2cap_connect_uid, bd_addr_result, proc_event_uid);
			append_line(line, bounded_line_length(n, sizeof(line)));
			break;
		}
		ksceKernelDelayThread(10000);
	}

	unsigned int command_poll = 0;
	while (!stop_requested) {
		expire_controller_heartbeat();
		handle_peer_address_save();
		handle_pending_link_key();
		handle_link_key_save();
		handle_l2cap_response();
		handle_pairing_reset();
		update_pairing_discoverable();
		handle_reconnect_request();
		handle_active_reconnect();
		send_input();
		const DiagnosticTraceEvent *entry =
			diagnostic_trace_event(read_index);
		unsigned int available_sequence = entry->sequence;
		if (available_sequence > read_index + 1) {
			int n = snprintf(line, sizeof(line),
				"trace_dropped=%u resume=%u\n",
				available_sequence - (read_index + 1), available_sequence);
			append_line(line, bounded_line_length(n, sizeof(line)));
			read_index = available_sequence - 1;
			__sync_synchronize();
			continue;
		}
		if (entry->sequence == read_index + 1) {
			unsigned int shown = entry->length > sizeof(entry->data) ? sizeof(entry->data) : entry->length;
			int n = snprintf(line, sizeof(line), "seq=%u type=%u len=%u data=",
				read_index + 1, entry->type, entry->length);
			for (unsigned int i = 0; i < shown && n < (int)sizeof(line) - 3; i++)
				n += snprintf(line + n, sizeof(line) - n, "%02X", entry->data[i]);
			line[n++] = '\n';
			append_line(line, bounded_line_length(n, sizeof(line)));
			read_index++;
		}
		ksceKernelDelayThread(5000);
		if (++command_poll == 200) {
			handle_command();
			command_poll = 0;
		}
	}
	return 0;
}

int module_start(SceSize argc, const void *args)
{
	(void)argc;
	(void)args;
	diagnostic_trace_reset();
	stop_requested = 0;
	controller_app_active = 0;
	controller_app_pid = -1;
	controller_heartbeat_time = 0;
	controller_vita_buttons = 0;
	controller_vita_sticks = 0x80808080U;
	controller_touch_buttons = 0;
	controller_input_sequence = 0;
	input_send_ready = 0;
	l2cap_response_pending = 0;
	switch_acl_handle = 0;
	bond_state_reset();
	switch_address_save_pending = 0;
	switch_address_save_attempted = 0;
	switch_link_key_save_pending = 0;
	switch_link_key_save_attempted = 0;
	pending_link_key_consumed = 0;
	active_reconnect_state = ACTIVE_RECONNECT_IDLE;
	active_reconnect_attempts = 0;
	active_reconnect_started = 0;
	active_reconnect_context = NULL;
	raw_acl_reconnect = 0;
	raw_transport_completions = 0;
	raw_init_script_stage = 0;
	reset_raw_control_channel();
	active_reconnect_scan_active = 0;
	active_reconnect_scan_started = 0;
	active_hid_start_call = 0;
	active_hid_start_connection = NULL;
	active_hid_start_channel = NULL;
	deferred_hid_connection = NULL;
	deferred_hid_control_channel = NULL;
	active_l2cap_connect_call = 0;
	last_switch_activity = 0;
	sender_stop_requested = 0;
	sender_request_pending = 0;
	sender_blocked = 0;
	sender_connection = NULL;
	sender_remote_cid = 0;
	sender_report_length = 0;
	last_input_report_time = 0;
	bootstrap_input_reports = 0;
	bootstrap_output_seen = 0;
	memset(last_sent_buttons, 0, sizeof(last_sent_buttons));
	last_sent_lx = last_sent_ly = last_sent_rx = last_sent_ry = 0;
	last_sent_battery_level = 0xff;
	last_battery_poll_time = 0;
	last_battery_percent = -1;
	last_battery_charging = -1;
	reconnect_requested = 0;
	reconnect_input_latched = 0;
	pairing_resume_requested = 0;
	pairing_reset_requested = 0;
	pairing_reset_state = 0;
	force_disconnect = 0;
	pairing_reset_started = 0;
	pairing_discoverable_active = 0;
	pairing_discoverable_started = 0;
	previous_vita_buttons = 0;
	previous_vita_sticks = 0x80808080U;
	previous_touch_buttons = 0;
	memset(controller_mac, 0, sizeof(controller_mac));
	worker_uid = ksceKernelCreateThread("scebt_trace_worker", trace_worker,
		0x10000100, 0x4000, 0, 0, NULL);
	if (worker_uid < 0) {
		return SCE_KERNEL_START_FAILED;
	}
	if (ksceKernelStartThread(worker_uid, 0, NULL) < 0) {
		ksceKernelDeleteThread(worker_uid);
		worker_uid = -1;
		return SCE_KERNEL_START_FAILED;
	}
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc;
	(void)args;
	stop_requested = 1;
	sender_stop_requested = 1;
	if (proc_event_uid >= 0) {
		ksceKernelUnregisterProcEventHandler(proc_event_uid);
		proc_event_uid = -1;
	}
	if (hci_event_uid >= 0) {
		taiHookReleaseForKernel(hci_event_uid, hci_event_ref);
		hci_event_uid = -1;
	}
	if (acl_event_uid >= 0) {
		taiHookReleaseForKernel(acl_event_uid, acl_event_ref);
		acl_event_uid = -1;
	}
	if (hid_channel_uid >= 0) {
		taiHookReleaseForKernel(hid_channel_uid, hid_channel_ref);
		hid_channel_uid = -1;
	}
	if (disconnect_direct_uid >= 0) {
		taiHookReleaseForKernel(disconnect_direct_uid, disconnect_direct_ref);
		disconnect_direct_uid = -1;
	}
	if (disconnect_cleanup_uid >= 0) {
		taiHookReleaseForKernel(disconnect_cleanup_uid, disconnect_cleanup_ref);
		disconnect_cleanup_uid = -1;
	}
	if (hid_disconnect_uid >= 0) {
		taiHookReleaseForKernel(hid_disconnect_uid, hid_disconnect_ref);
		hid_disconnect_uid = -1;
	}
	if (hci_command_uid >= 0) {
		taiHookReleaseForKernel(hci_command_uid, hci_command_ref);
		hci_command_uid = -1;
	}
	if (l2cap_connect_uid >= 0) {
		taiHookReleaseForKernel(l2cap_connect_uid, l2cap_connect_ref);
		l2cap_connect_uid = -1;
	}
	if (sdp_db_uid >= 0)
		taiInjectReleaseForKernel(sdp_db_uid);
	if (pnp_ids_uid >= 0)
		taiInjectReleaseForKernel(pnp_ids_uid);
	if (scan_mask_uid >= 0)
		taiInjectReleaseForKernel(scan_mask_uid);
	if (class_shift_uid >= 0)
		taiInjectReleaseForKernel(class_shift_uid);
	if (auth_requirements_uid >= 0)
		taiInjectReleaseForKernel(auth_requirements_uid);
	if (io_capability_uid >= 0)
		taiInjectReleaseForKernel(io_capability_uid);
	if (class_movw_uid >= 0)
		taiInjectReleaseForKernel(class_movw_uid);
	if (local_name_uid >= 0)
		taiInjectReleaseForKernel(local_name_uid);
	if (worker_uid >= 0) {
		ksceKernelWaitThreadEnd(worker_uid, NULL, NULL);
		ksceKernelDeleteThread(worker_uid);
		worker_uid = -1;
	}
	if (sender_uid >= 0) {
		ksceKernelWaitThreadEnd(sender_uid, NULL, NULL);
		ksceKernelDeleteThread(sender_uid);
		sender_uid = -1;
	}
	return SCE_KERNEL_STOP_SUCCESS;
}
