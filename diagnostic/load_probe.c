#include <psp2kern/io/fcntl.h>
#include <psp2kern/kernel/modulemgr.h>

int _start(SceSize argc, const void *args)
	__attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	static const char message[] = "module_start reached\n";
	(void)argc;
	(void)args;

	SceUID fd = ksceIoOpen("ur0:data/scebt-load-probe.txt",
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return SCE_KERNEL_START_FAILED;

	int result = ksceIoWrite(fd, message, sizeof(message) - 1);
	ksceIoClose(fd);
	return result == (int)(sizeof(message) - 1)
		? SCE_KERNEL_START_SUCCESS : SCE_KERNEL_START_FAILED;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc;
	(void)args;
	return SCE_KERNEL_STOP_SUCCESS;
}
