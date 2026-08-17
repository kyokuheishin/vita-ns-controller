#include <psp2kern/io/fcntl.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

static SceUID worker_uid = -1;
static volatile int stop_requested;

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

static int worker(SceSize argc, void *args)
{
	(void)argc;
	(void)args;
	write_marker("worker started\n", 15);

	for (int i = 0; i < 120 && !stop_requested; i++) {
		tai_module_info_t info;
		info.size = sizeof(info);
		if (taiGetModuleInfoForKernel(KERNEL_PID, "SceBt", &info) >= 0) {
			write_marker("SceBt found\n", 12);
			return 0;
		}
		ksceKernelDelayThread(500000);
	}

	write_marker("SceBt timeout\n", 14);
	return 0;
}

int module_start(SceSize argc, const void *args)
{
	(void)argc;
	(void)args;
	write_marker("module_start reached\n", 21);

	stop_requested = 0;
	worker_uid = ksceKernelCreateThread("scebt_lookup_probe", worker,
		0x10000100, 0x4000, 0, 0, 0);
	if (worker_uid < 0) {
		write_marker("thread create failed\n", 21);
		return SCE_KERNEL_START_FAILED;
	}
	if (ksceKernelStartThread(worker_uid, 0, 0) < 0) {
		write_marker("thread start failed\n", 20);
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
	if (worker_uid >= 0) {
		ksceKernelWaitThreadEnd(worker_uid, 0, 0);
		ksceKernelDeleteThread(worker_uid);
		worker_uid = -1;
	}
	return SCE_KERNEL_STOP_SUCCESS;
}
