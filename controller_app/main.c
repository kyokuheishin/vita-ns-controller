#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/touch.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vita2d.h>

#include "touch_map.h"
#include "vita_ns_pad.h"

#define COLOR_BG       RGBA8(14, 18, 28, 255)
#define COLOR_PANEL    RGBA8(38, 46, 60, 255)
#define COLOR_BORDER   RGBA8(92, 106, 128, 255)
#define COLOR_ACTIVE   RGBA8(26, 132, 220, 255)
#define COLOR_BUTTON   RGBA8(45, 55, 72, 255)
#define COLOR_PRESSED  RGBA8(28, 176, 112, 255)
#define COLOR_TEXT     RGBA8(242, 246, 255, 255)
#define COLOR_MUTED    RGBA8(170, 183, 204, 255)
#define PAIR_X         720
#define PAIR_Y         18
#define PAIR_WIDTH     212
#define PAIR_HEIGHT    52
#define PAIR_HOLD_FRAMES 120
#define LAYOUT_INFO_X  540
#define LAYOUT_FULL_X  400
#define LAYOUT_Y       18
#define LAYOUT_WIDTH   160
#define LAYOUT_HEIGHT  52
#define SETTINGS_X     360
#define SETTINGS_Y     18
#define SETTINGS_FULL_X 400
#define SETTINGS_FULL_Y 88
#define SETTINGS_WIDTH 160
#define SETTINGS_HEIGHT 52
#define SETTINGS_TOGGLE_X 120
#define SETTINGS_TOGGLE_WIDTH 720
#define SETTINGS_TOGGLE_HEIGHT 88
#define SETTINGS_LEFT_Y 130
#define SETTINGS_RIGHT_Y 242
#define SETTINGS_BACK_X 380
#define SETTINGS_BACK_Y 382
#define SETTINGS_BACK_WIDTH 200
#define SETTINGS_BACK_HEIGHT 64
#define CONFIG_PATH "ux0:app/VITANSPAD/config.bin"
#define LEGACY_CONFIG_PATH "ux0:data/vita-ns-pad.cfg"
#define CONFIG_MAGIC 0x434E5356U

enum AppScreen {
	APP_SCREEN_MAIN = 0,
	APP_SCREEN_SETTINGS,
};

static void stop_input_sampling(void)
{
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
		SCE_TOUCH_SAMPLING_STATE_STOP);
	vitaNsPadDisable();
}

static int read_options(const char *path, uint8_t *options)
{
	uint32_t data[2] = { 0, 0 };
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return fd;
	int length = sceIoRead(fd, data, sizeof(data));
	sceIoClose(fd);
	if (length != (int)sizeof(data) || data[0] != CONFIG_MAGIC)
		return -1;
	*options = (uint8_t)data[1] & VITA_NS_TOUCH_OPTION_MASK;
	return 0;
}

static int save_options(uint8_t options)
{
	uint32_t data[2] = { CONFIG_MAGIC, options & VITA_NS_TOUCH_OPTION_MASK };
	SceUID fd = sceIoOpen(CONFIG_PATH,
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return fd;
	int result = sceIoWrite(fd, data, sizeof(data));
	int close_result = sceIoClose(fd);
	if (result != (int)sizeof(data))
		return result < 0 ? result : -1;
	return close_result;
}

static uint8_t load_options(void)
{
	uint8_t options = 0;
	if (read_options(CONFIG_PATH, &options) >= 0)
		return options;
	if (read_options(LEGACY_CONFIG_PATH, &options) >= 0) {
		/* Keep the legacy file as a recovery copy until migration has been
		 * verified on the device. */
		save_options(options);
		return options;
	}
	return 0;
}

static uint8_t read_touch_buttons(SceTouchData *touch, int layout)
{
	uint8_t buttons = 0;
	touch->reportNum = 0;
	if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, touch, 1) < 1)
		return 0;
	for (SceUInt32 i = 0; i < touch->reportNum && i < SCE_TOUCH_MAX_REPORT; i++)
		buttons |= vita_ns_touch_button_for_layout(touch->report[i].x,
			touch->report[i].y, layout);
	return buttons;
}

static int layout_touch_active(const SceTouchData *touch, int layout)
{
	int button_x = layout == VITA_NS_TOUCH_LAYOUT_FULL ?
		LAYOUT_FULL_X : LAYOUT_INFO_X;
	for (SceUInt32 i = 0; i < touch->reportNum && i < SCE_TOUCH_MAX_REPORT; i++) {
		int x = touch->report[i].x / 2;
		int y = touch->report[i].y / 2;
		if (x >= button_x && x < button_x + LAYOUT_WIDTH &&
		    y >= LAYOUT_Y && y < LAYOUT_Y + LAYOUT_HEIGHT)
			return 1;
	}
	return 0;
}

static int touch_rect_active(const SceTouchData *touch, int left, int top,
	int width, int height)
{
	for (SceUInt32 i = 0; i < touch->reportNum && i < SCE_TOUCH_MAX_REPORT; i++) {
		int x = touch->report[i].x / 2;
		int y = touch->report[i].y / 2;
		if (x >= left && x < left + width &&
		    y >= top && y < top + height)
			return 1;
	}
	return 0;
}

static int settings_touch_active(const SceTouchData *touch, int layout)
{
	int x = layout == VITA_NS_TOUCH_LAYOUT_FULL ? SETTINGS_FULL_X : SETTINGS_X;
	int y = layout == VITA_NS_TOUCH_LAYOUT_FULL ? SETTINGS_FULL_Y : SETTINGS_Y;
	return touch_rect_active(touch, x, y,
		SETTINGS_WIDTH, SETTINGS_HEIGHT);
}

static int pair_touch_active(const SceTouchData *touch)
{
	for (SceUInt32 i = 0; i < touch->reportNum && i < SCE_TOUCH_MAX_REPORT; i++) {
		int x = touch->report[i].x / 2;
		int y = touch->report[i].y / 2;
		if (x >= PAIR_X && x < PAIR_X + PAIR_WIDTH &&
		    y >= PAIR_Y && y < PAIR_Y + PAIR_HEIGHT)
			return 1;
	}
	return 0;
}

static void draw_centered(vita2d_pgf *font, int x, int width, int baseline,
	float scale, unsigned int color, const char *text)
{
	int text_width = vita2d_pgf_text_width(font, scale, text);
	vita2d_pgf_draw_text(font, x + (width - text_width) / 2,
		baseline, color, scale, text);
}

static void draw_indicator(vita2d_pgf *font, int x, int y,
	const char *label, int pressed)
{
	const int width = 106;
	const int height = 31;
	vita2d_draw_rectangle(x, y, width, height,
		pressed ? COLOR_PRESSED : COLOR_BUTTON);
	draw_centered(font, x, width, y + 23, 0.75f,
		pressed ? COLOR_TEXT : COLOR_MUTED, label);
}

static void draw_touch_zones(vita2d_pgf *font, uint8_t buttons, int layout,
	uint8_t options)
{
	static const struct {
		const char *label;
		uint8_t mask;
		int x;
		int y;
		int width;
		int height;
	} info_zones[VITA_NS_TOUCH_ZONE_COUNT] = {
		{"ZL", VITA_NS_TOUCH_ZL, 0, VITA_NS_TOUCH_SCREEN_Y,
			VITA_NS_TOUCH_SCREEN_COLUMN_WIDTH, VITA_NS_TOUCH_SCREEN_ROW_HEIGHT},
		{"L3", VITA_NS_TOUCH_L3, 0,
			VITA_NS_TOUCH_SCREEN_Y + VITA_NS_TOUCH_SCREEN_ROW_HEIGHT,
			VITA_NS_TOUCH_SCREEN_COLUMN_WIDTH, VITA_NS_TOUCH_SCREEN_ROW_HEIGHT},
		{"CAPTURE", VITA_NS_TOUCH_CAPTURE, VITA_NS_TOUCH_SCREEN_COLUMN_WIDTH,
			VITA_NS_TOUCH_SCREEN_Y, VITA_NS_TOUCH_SCREEN_COLUMN_WIDTH,
			VITA_NS_TOUCH_SCREEN_ROW_HEIGHT * 2},
		{"HOME", VITA_NS_TOUCH_HOME, VITA_NS_TOUCH_SCREEN_COLUMN_WIDTH * 2,
			VITA_NS_TOUCH_SCREEN_Y, VITA_NS_TOUCH_SCREEN_COLUMN_WIDTH,
			VITA_NS_TOUCH_SCREEN_ROW_HEIGHT * 2},
		{"ZR", VITA_NS_TOUCH_ZR, VITA_NS_TOUCH_SCREEN_COLUMN_WIDTH * 3,
			VITA_NS_TOUCH_SCREEN_Y, VITA_NS_TOUCH_SCREEN_COLUMN_WIDTH,
			VITA_NS_TOUCH_SCREEN_ROW_HEIGHT},
		{"R3", VITA_NS_TOUCH_R3, VITA_NS_TOUCH_SCREEN_COLUMN_WIDTH * 3,
			VITA_NS_TOUCH_SCREEN_Y + VITA_NS_TOUCH_SCREEN_ROW_HEIGHT,
			VITA_NS_TOUCH_SCREEN_COLUMN_WIDTH, VITA_NS_TOUCH_SCREEN_ROW_HEIGHT},
	};
	static const struct {
		const char *label;
		uint8_t mask;
		int x;
		int y;
		int width;
		int height;
	} full_zones[VITA_NS_TOUCH_ZONE_COUNT] = {
		{"ZL", VITA_NS_TOUCH_ZL, 0, 0,
			VITA_NS_TOUCH_FULL_SIDE_WIDTH / 2,
			VITA_NS_TOUCH_FULL_CORNER_HEIGHT / 2},
		{"L3", VITA_NS_TOUCH_L3, 0, VITA_NS_TOUCH_FULL_BOTTOM_Y / 2,
			VITA_NS_TOUCH_FULL_SIDE_WIDTH / 2,
			(VITA_NS_TOUCH_PANEL_HEIGHT - VITA_NS_TOUCH_FULL_BOTTOM_Y) / 2},
		{"CAPTURE", VITA_NS_TOUCH_CAPTURE, VITA_NS_TOUCH_FULL_CAPTURE_X / 2,
			VITA_NS_TOUCH_FULL_CENTER_Y / 2,
			VITA_NS_TOUCH_FULL_CENTER_SIZE / 2,
			VITA_NS_TOUCH_FULL_CENTER_SIZE / 2},
		{"HOME", VITA_NS_TOUCH_HOME, VITA_NS_TOUCH_FULL_HOME_X / 2,
			VITA_NS_TOUCH_FULL_CENTER_Y / 2,
			VITA_NS_TOUCH_FULL_CENTER_SIZE / 2,
			VITA_NS_TOUCH_FULL_CENTER_SIZE / 2},
		{"ZR", VITA_NS_TOUCH_ZR, VITA_NS_TOUCH_FULL_RIGHT_X / 2, 0,
			(VITA_NS_TOUCH_PANEL_WIDTH - VITA_NS_TOUCH_FULL_RIGHT_X) / 2,
			VITA_NS_TOUCH_FULL_CORNER_HEIGHT / 2},
		{"R3", VITA_NS_TOUCH_R3, VITA_NS_TOUCH_FULL_RIGHT_X / 2,
			VITA_NS_TOUCH_FULL_BOTTOM_Y / 2,
			(VITA_NS_TOUCH_PANEL_WIDTH - VITA_NS_TOUCH_FULL_RIGHT_X) / 2,
			(VITA_NS_TOUCH_PANEL_HEIGHT - VITA_NS_TOUCH_FULL_BOTTOM_Y) / 2},
	};
	for (int zone = 0; zone < VITA_NS_TOUCH_ZONE_COUNT; zone++) {
		const int full = layout == VITA_NS_TOUCH_LAYOUT_FULL;
		const char *label = full ? full_zones[zone].label : info_zones[zone].label;
		uint8_t mask = full ? full_zones[zone].mask : info_zones[zone].mask;
		if (mask == VITA_NS_TOUCH_ZL && (options & VITA_NS_TOUCH_SWAP_LEFT))
			label = "L";
		if (mask == VITA_NS_TOUCH_ZR && (options & VITA_NS_TOUCH_SWAP_RIGHT))
			label = "R";
		int x = full ? full_zones[zone].x : info_zones[zone].x;
		int y = full ? full_zones[zone].y : info_zones[zone].y;
		int width = full ? full_zones[zone].width : info_zones[zone].width;
		int height = full ? full_zones[zone].height : info_zones[zone].height;
		int active = buttons & mask;
		vita2d_draw_rectangle(x, y, width, height, COLOR_BORDER);
		vita2d_draw_rectangle(x + 3, y + 3, width - 6, height - 6,
			active ? COLOR_ACTIVE : COLOR_PANEL);
		draw_centered(font, x, width, y + height / 2 + 3, 1.0f,
			active ? COLOR_TEXT : COLOR_MUTED, label);
		draw_centered(font, x, width, y + height / 2 + 31, 0.65f,
			active ? COLOR_TEXT : COLOR_MUTED,
			active ? "PRESSED" : "TOUCH");
	}
}

static void draw_layout_button(vita2d_pgf *font, int layout)
{
	int x = layout == VITA_NS_TOUCH_LAYOUT_FULL ? LAYOUT_FULL_X : LAYOUT_INFO_X;
	vita2d_draw_rectangle(x, LAYOUT_Y, LAYOUT_WIDTH, LAYOUT_HEIGHT, COLOR_BORDER);
	vita2d_draw_rectangle(x + 3, LAYOUT_Y + 3,
		LAYOUT_WIDTH - 6, LAYOUT_HEIGHT - 6, COLOR_BUTTON);
	draw_centered(font, x, LAYOUT_WIDTH, LAYOUT_Y + 32, 0.68f, COLOR_TEXT,
		layout == VITA_NS_TOUCH_LAYOUT_FULL ? "SHOW INFO" : "FULL CONTROLS");
}

static void draw_settings_button(vita2d_pgf *font, int layout)
{
	int x = layout == VITA_NS_TOUCH_LAYOUT_FULL ? SETTINGS_FULL_X : SETTINGS_X;
	int y = layout == VITA_NS_TOUCH_LAYOUT_FULL ? SETTINGS_FULL_Y : SETTINGS_Y;
	vita2d_draw_rectangle(x, y,
		SETTINGS_WIDTH, SETTINGS_HEIGHT, COLOR_BORDER);
	vita2d_draw_rectangle(x + 3, y + 3,
		SETTINGS_WIDTH - 6, SETTINGS_HEIGHT - 6, COLOR_BUTTON);
	draw_centered(font, x, SETTINGS_WIDTH, y + 32,
		0.68f, COLOR_TEXT, "SETTINGS");
}

static void draw_setting_toggle(vita2d_pgf *font, int y,
	const char *title, const char *description, int enabled)
{
	unsigned int fill = enabled ? COLOR_PRESSED : COLOR_BUTTON;
	vita2d_draw_rectangle(SETTINGS_TOGGLE_X, y,
		SETTINGS_TOGGLE_WIDTH, SETTINGS_TOGGLE_HEIGHT, COLOR_BORDER);
	vita2d_draw_rectangle(SETTINGS_TOGGLE_X + 3, y + 3,
		SETTINGS_TOGGLE_WIDTH - 6, SETTINGS_TOGGLE_HEIGHT - 6, fill);
	vita2d_pgf_draw_text(font, SETTINGS_TOGGLE_X + 24, y + 34,
		COLOR_TEXT, 0.86f, title);
	vita2d_pgf_draw_text(font, SETTINGS_TOGGLE_X + 24, y + 65,
		enabled ? COLOR_TEXT : COLOR_MUTED, 0.66f, description);
	draw_centered(font, SETTINGS_TOGGLE_X + SETTINGS_TOGGLE_WIDTH - 112,
		88, y + 50, 0.8f, COLOR_TEXT, enabled ? "ON" : "OFF");
}

static void draw_settings(vita2d_pgf *font, uint8_t options, int save_result)
{
	vita2d_pgf_draw_text(font, 40, 50, COLOR_TEXT, 1.2f,
		"Controller Mapping Settings");
	vita2d_pgf_draw_text(font, 40, 88, COLOR_MUTED, 0.68f,
		"Left and right swaps are independent and saved immediately.");
	draw_setting_toggle(font, SETTINGS_LEFT_Y, "L <-> ZL",
		(options & VITA_NS_TOUCH_SWAP_LEFT) ?
		"Vita L -> ZL    Touch ZL -> L" :
		"Vita L -> L     Touch ZL -> ZL",
		options & VITA_NS_TOUCH_SWAP_LEFT);
	draw_setting_toggle(font, SETTINGS_RIGHT_Y, "R <-> ZR",
		(options & VITA_NS_TOUCH_SWAP_RIGHT) ?
		"Vita R -> ZR    Touch ZR -> R" :
		"Vita R -> R     Touch ZR -> ZR",
		options & VITA_NS_TOUCH_SWAP_RIGHT);
	vita2d_draw_rectangle(SETTINGS_BACK_X, SETTINGS_BACK_Y,
		SETTINGS_BACK_WIDTH, SETTINGS_BACK_HEIGHT, COLOR_BORDER);
	vita2d_draw_rectangle(SETTINGS_BACK_X + 3, SETTINGS_BACK_Y + 3,
		SETTINGS_BACK_WIDTH - 6, SETTINGS_BACK_HEIGHT - 6, COLOR_BUTTON);
	draw_centered(font, SETTINGS_BACK_X, SETTINGS_BACK_WIDTH,
		SETTINGS_BACK_Y + 40, 0.8f, COLOR_TEXT, "BACK");
	if (save_result < 0)
		draw_centered(font, 0, VITA_NS_TOUCH_SCREEN_WIDTH, 486,
			0.68f, COLOR_ACTIVE, "Could not save app config.bin");
}

static void draw_pair_button(vita2d_pgf *font, unsigned int hold_frames,
	int queued)
{
	unsigned int fill = queued ? COLOR_PRESSED :
		(hold_frames ? COLOR_ACTIVE : COLOR_BUTTON);
	vita2d_draw_rectangle(PAIR_X, PAIR_Y, PAIR_WIDTH, PAIR_HEIGHT,
		COLOR_BORDER);
	vita2d_draw_rectangle(PAIR_X + 3, PAIR_Y + 3,
		PAIR_WIDTH - 6, PAIR_HEIGHT - 6, fill);
	draw_centered(font, PAIR_X, PAIR_WIDTH, PAIR_Y + 24, 0.68f,
		COLOR_TEXT, queued ? "PAIRING QUEUED" : "HOLD: NEW PAIR");
	float progress = hold_frames >= PAIR_HOLD_FRAMES ? 1.0f :
		(float)hold_frames / PAIR_HOLD_FRAMES;
	vita2d_draw_rectangle(PAIR_X + 9, PAIR_Y + 38,
		PAIR_WIDTH - 18, 6, COLOR_PANEL);
	vita2d_draw_rectangle(PAIR_X + 9, PAIR_Y + 38,
		(PAIR_WIDTH - 18) * progress, 6, COLOR_TEXT);
}

static void draw_ui(vita2d_pgf *font, const SceCtrlData *pad,
	uint8_t touch_buttons, unsigned int exit_frames, int heartbeat_result,
	unsigned int pair_hold_frames, int pair_queued, int layout,
	int screen, uint8_t options, int save_result)
{
	vita2d_clear_screen();
	if (screen == APP_SCREEN_SETTINGS) {
		draw_settings(font, options, save_result);
		return;
	}
	if (layout == VITA_NS_TOUCH_LAYOUT_FULL) {
		draw_touch_zones(font, touch_buttons, layout, options);
		draw_layout_button(font, layout);
		draw_settings_button(font, layout);
		return;
	}
	vita2d_pgf_draw_text(font, 28, 38, COLOR_TEXT, 1.15f,
		"Vita NS Controller");
	vita2d_pgf_draw_text(font, 28, 66,
		heartbeat_result >= 0 ? COLOR_PRESSED : COLOR_ACTIVE, 0.72f,
		heartbeat_result >= 0 ? "Kernel input gate: ACTIVE" :
		"Kernel input gate: ERROR");

	static const struct {
		const char *label;
		uint32_t mask;
	} indicators[12] = {
		{"Y / Square", SCE_CTRL_SQUARE}, {"X / Triangle", SCE_CTRL_TRIANGLE},
		{"B / Cross", SCE_CTRL_CROSS}, {"A / Circle", SCE_CTRL_CIRCLE},
		{"D-pad Up", SCE_CTRL_UP}, {"D-pad Right", SCE_CTRL_RIGHT},
		{"D-pad Down", SCE_CTRL_DOWN}, {"D-pad Left", SCE_CTRL_LEFT},
		{"L", SCE_CTRL_LTRIGGER}, {"R", SCE_CTRL_RTRIGGER},
		{"Minus", SCE_CTRL_SELECT}, {"Plus", SCE_CTRL_START},
	};
	for (int i = 0; i < 12; i++) {
		int column = i % 6;
		int row = i / 6;
		const char *label = indicators[i].label;
		if (i == 8 && (options & VITA_NS_TOUCH_SWAP_LEFT))
			label = "ZL [L]";
		if (i == 9 && (options & VITA_NS_TOUCH_SWAP_RIGHT))
			label = "ZR [R]";
		draw_indicator(font, 28 + column * 119, 83 + row * 42,
			label, pad->buttons & indicators[i].mask);
	}

	vita2d_pgf_draw_textf(font, 28, 190, COLOR_TEXT, 0.72f,
		"Left stick  %3u, %3u", pad->lx, pad->ly);
	vita2d_pgf_draw_textf(font, 265, 190, COLOR_TEXT, 0.72f,
		"Right stick  %3u, %3u", pad->rx, pad->ry);
	if (options)
		vita2d_pgf_draw_textf(font, 28, 222, COLOR_MUTED, 0.68f,
			"Swap active: L/ZL %s   R/ZR %s",
			(options & VITA_NS_TOUCH_SWAP_LEFT) ? "ON" : "OFF",
			(options & VITA_NS_TOUCH_SWAP_RIGHT) ? "ON" : "OFF");
	else
		vita2d_pgf_draw_text(font, 28, 222, COLOR_MUTED, 0.68f,
			"Touch: ZL/ZR corners; L3/R3 corners; Capture | Home");
	vita2d_pgf_draw_text(font, 28, 250, COLOR_MUTED, 0.65f,
		"Hold Select + Start for 2 seconds to exit");
	if (exit_frames) {
		float progress = exit_frames >= 120 ? 1.0f : (float)exit_frames / 120.0f;
		vita2d_draw_rectangle(600, 235, 320, 10, COLOR_BUTTON);
		vita2d_draw_rectangle(600, 235, 320.0f * progress, 10, COLOR_ACTIVE);
	}
	draw_pair_button(font, pair_hold_frames, pair_queued);
	draw_settings_button(font, layout);
	draw_layout_button(font, layout);
	draw_touch_zones(font, touch_buttons, layout, options);
}

int main(void)
{
	SceCtrlData pad;
	SceTouchData touch;
	unsigned int exit_frames = 0;
	unsigned int pair_hold_frames = 0;
	int pair_queued = 0;
	int layout = VITA_NS_TOUCH_LAYOUT_INFO;
	int screen = APP_SCREEN_MAIN;
	int ui_touch_latched = 0;
	uint8_t options = load_options();
	int save_result = 0;
	memset(&pad, 0, sizeof(pad));
	memset(&touch, 0, sizeof(touch));

	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
		SCE_TOUCH_SAMPLING_STATE_START);
	if (vita2d_init() < 0) {
		stop_input_sampling();
		return 1;
	}
	vita2d_set_vblank_wait(1);
	vita2d_set_clear_color(COLOR_BG);
	vita2d_pgf *font = vita2d_load_default_pgf();
	if (!font) {
		vita2d_fini();
		stop_input_sampling();
		return 1;
	}

	for (;;) {
		if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1) {
			pad.buttons = 0;
			pad.lx = pad.ly = pad.rx = pad.ry = 128;
		}
		uint8_t touch_buttons = read_touch_buttons(&touch, layout);
		if (!touch.reportNum)
			ui_touch_latched = 0;
		if (!ui_touch_latched) {
			if (screen == APP_SCREEN_SETTINGS) {
				if (touch_rect_active(&touch, SETTINGS_TOGGLE_X, SETTINGS_LEFT_Y,
				    SETTINGS_TOGGLE_WIDTH, SETTINGS_TOGGLE_HEIGHT)) {
					options ^= VITA_NS_TOUCH_SWAP_LEFT;
					save_result = save_options(options);
					ui_touch_latched = 1;
				} else if (touch_rect_active(&touch, SETTINGS_TOGGLE_X,
				    SETTINGS_RIGHT_Y, SETTINGS_TOGGLE_WIDTH,
				    SETTINGS_TOGGLE_HEIGHT)) {
					options ^= VITA_NS_TOUCH_SWAP_RIGHT;
					save_result = save_options(options);
					ui_touch_latched = 1;
				} else if (touch_rect_active(&touch, SETTINGS_BACK_X,
				    SETTINGS_BACK_Y, SETTINGS_BACK_WIDTH,
				    SETTINGS_BACK_HEIGHT)) {
					screen = APP_SCREEN_MAIN;
					ui_touch_latched = 1;
				}
			} else if (layout_touch_active(&touch, layout)) {
				layout = layout == VITA_NS_TOUCH_LAYOUT_INFO ?
					VITA_NS_TOUCH_LAYOUT_FULL : VITA_NS_TOUCH_LAYOUT_INFO;
				ui_touch_latched = 1;
				pair_hold_frames = 0;
				pair_queued = 0;
			} else if (settings_touch_active(&touch, layout)) {
				screen = APP_SCREEN_SETTINGS;
				ui_touch_latched = 1;
				pair_hold_frames = 0;
				pair_queued = 0;
			}
		}
		if (ui_touch_latched || screen == APP_SCREEN_SETTINGS)
			touch_buttons = 0;
		int pair_touched = screen == APP_SCREEN_MAIN &&
			layout == VITA_NS_TOUCH_LAYOUT_INFO &&
			!ui_touch_latched && pair_touch_active(&touch);
		uint32_t sticks = (uint32_t)pad.lx | ((uint32_t)pad.ly << 8) |
			((uint32_t)pad.rx << 16) | ((uint32_t)pad.ry << 24);
		int heartbeat_result = vitaNsPadSubmitInput(pad.buttons, sticks,
			touch_buttons | options);
		if (pair_touched) {
			if (pair_hold_frames < PAIR_HOLD_FRAMES)
				pair_hold_frames++;
			if (pair_hold_frames >= PAIR_HOLD_FRAMES && !pair_queued)
				pair_queued = vitaNsPadStartPairing() >= 0;
		} else {
			pair_hold_frames = 0;
			pair_queued = 0;
		}
		if ((pad.buttons & (SCE_CTRL_SELECT | SCE_CTRL_START)) ==
		    (SCE_CTRL_SELECT | SCE_CTRL_START)) {
			if (exit_frames < 120)
				exit_frames++;
		} else {
			exit_frames = 0;
		}

		vita2d_start_drawing();
		draw_ui(font, &pad, touch_buttons, exit_frames, heartbeat_result,
			pair_hold_frames, pair_queued, layout, screen, options,
			save_result);
		vita2d_end_drawing();
		vita2d_swap_buffers();
		if (exit_frames >= 120)
			break;
	}

	vita2d_wait_rendering_done();
	vita2d_free_pgf(font);
	vita2d_fini();
	stop_input_sampling();
	return 0;
}
