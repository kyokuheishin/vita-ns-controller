#ifndef VITA_NS_PAD_H
#define VITA_NS_PAD_H

#include <stdint.h>

#include "motion_sample.h"

int vitaNsPadSubmitInput(uint32_t buttons, uint32_t sticks,
	uint32_t touch_buttons);
int vitaNsPadSubmitMotion(
	const VitaNsMotionSample samples[VITA_NS_MOTION_SAMPLE_COUNT],
	uint32_t sample_count);
int vitaNsPadStartPairing(void);
int vitaNsPadDisable(void);

#endif
