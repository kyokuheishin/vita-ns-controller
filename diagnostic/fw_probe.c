#include <psp2kern/io/fcntl.h>
#include <psp2kern/kernel/modulemgr.h>

int _start(SceSize argc, const void *args)
	__attribute__((weak, alias("module_start")));

static void write_marker(const char *message, SceSize length)
{
	SceUID fd = ksceIoOpen("ur0:data/scebt-load-probe.txt",
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd >= 0) {
		ksceIoWrite(fd, message, length);
		ksceIoClose(fd);
	}
}

int module_start(SceSize argc, const void *args)
{
	SceKernelFwInfo info;
	(void)argc;
	(void)args;

	info.size = sizeof(info);
	if (ksceKernelGetSystemSwVersion(&info) < 0) {
		write_marker("fwinfo call failed\n", 19);
		return SCE_KERNEL_START_FAILED;
	}

	SceUID fd = ksceIoOpen("ur0:data/scebt-fwinfo.bin",
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0) {
		write_marker("fwinfo open failed\n", 19);
		return SCE_KERNEL_START_FAILED;
	}

	int result = ksceIoWrite(fd, &info, sizeof(info));
	ksceIoClose(fd);
	if (result != (int)sizeof(info)) {
		write_marker("fwinfo write failed\n", 20);
		return SCE_KERNEL_START_FAILED;
	}

	write_marker("fwinfo written\n", 15);
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc;
	(void)args;
	return SCE_KERNEL_STOP_SUCCESS;
}
