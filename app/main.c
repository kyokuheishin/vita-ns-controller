#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <string.h>

#include "debugScreen.h"

#define TRACE_PATH "ux0:data/scebt-trace.txt"
#define COMMAND_PATH "ux0:data/scebt-command.txt"
#define printf psvDebugScreenPrintf

static int send_command(const char *command)
{
	SceUID fd = sceIoOpen(COMMAND_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return fd;
	int length = strlen(command);
	int result = sceIoWrite(fd, command, length);
	sceIoClose(fd);
	return result == length ? 0 : result;
}

static char *read_trace(char *buffer, int capacity)
{
	SceUID fd = sceIoOpen(TRACE_PATH, SCE_O_RDONLY, 0);
	if (fd < 0) {
		snprintf(buffer, capacity, "trace unavailable: 0x%08X\n", fd);
		return buffer;
	}
	SceOff end = sceIoLseek(fd, 0, SCE_SEEK_END);
	SceOff start = end > capacity - 1 ? end - (capacity - 1) : 0;
	sceIoLseek(fd, start, SCE_SEEK_SET);
	int length = sceIoRead(fd, buffer, capacity - 1);
	sceIoClose(fd);
	if (length < 0) {
		snprintf(buffer, capacity, "trace read failed: 0x%08X\n", length);
		return buffer;
	}
	buffer[length] = 0;
	char *view = buffer;
	int lines = 0;
	for (int i = length - 1; i >= 0; i--) {
		if (buffer[i] == '\n' && ++lines == 18) {
			view = buffer + i + 1;
			break;
		}
	}
	return view;
}

int main(void)
{
	SceCtrlData pad = {0};
	unsigned int previous = 0;
	char trace[4096];
	char status[96] = "Ready";

	psvDebugScreenInit();
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
	for (;;) {
		sceCtrlPeekBufferPositive(0, &pad, 1);
		unsigned int pressed = pad.buttons & ~previous;
		previous = pad.buttons;
		if (pressed & SCE_CTRL_START)
			break;
		if (pressed & SCE_CTRL_CROSS) {
			int result = send_command("apply\n");
			snprintf(status, sizeof(status), result < 0 ?
				"Apply failed: 0x%08X" : "Apply queued; wait about one second", result);
		}
		if (pressed & SCE_CTRL_SQUARE) {
			int result = send_command("clear\n");
			snprintf(status, sizeof(status), result < 0 ?
				"Clear failed: 0x%08X" : "Clear queued", result);
		}
		if (pressed & SCE_CTRL_TRIANGLE) {
			int result = send_command("forget\n");
			snprintf(status, sizeof(status), result < 0 ?
				"Forget failed: 0x%08X" : "Forget Switch queued; wait about one second", result);
		}

		psvDebugScreenClear(0x101018);
		printf("SceBt Pro Controller Test\n\n");
		printf("[X] Reapply name / CoD / discoverable scan\n");
		printf("[Triangle] Forget Switch link key only\n");
		printf("[Square] Clear trace    [Start] Exit\n");
		printf("Do not open the system Bluetooth settings while testing.\n");
		printf("Status: %s\n\n", status);
		printf("--- ux0:data/scebt-trace.txt (tail) ---\n%s", read_trace(trace, sizeof(trace)));
		sceKernelDelayThread(250000);
	}
	return 0;
}
