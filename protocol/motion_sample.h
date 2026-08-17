#ifndef VITA_NS_MOTION_SAMPLE_H
#define VITA_NS_MOTION_SAMPLE_H

#include <stdint.h>

#define VITA_NS_MOTION_SAMPLE_COUNT 3

typedef struct {
	int16_t accel[3];
	int16_t gyro[3];
} VitaNsMotionSample;

#endif
