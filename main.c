#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>
#include <stdint.h>
#include <taihen.h>

#define UX0_DUMP_DIR "ux0:data/scebt-probe"
#define UR0_DUMP_DIR "ur0:data/scebt-probe"
#define CHUNK_SIZE 0x10000

static const char *dump_dir;
static SceUID worker_uid = -1;
static volatile int stop_requested;

int _start(SceSize argc, const void *args) __attribute__((weak, alias("module_start")));

static int prepare_dump_dir(const char *path)
{
	if (ksceIoMkdir(path, 0777) >= 0)
		return 0;

	SceIoStat stat;
	memset(&stat, 0, sizeof(stat));
	return ksceIoGetstat(path, &stat) >= 0 && SCE_S_ISDIR(stat.st_mode) ? 0 : -1;
}

static int write_all(SceUID fd, const void *data, SceSize size)
{
	const uint8_t *p = data;

	while (size) {
		SceSize chunk = size > CHUNK_SIZE ? CHUNK_SIZE : size;
		int written = ksceIoWrite(fd, p, chunk);
		if (written <= 0)
			return written < 0 ? written : -1;
		p += written;
		size -= written;
	}
	return 0;
}

static void write_status(const char *phase, int result)
{
	char text[96];
	int length = snprintf(text, sizeof(text), "phase=%s\nresult=%d\n", phase, result);
	if (length <= 0 || length >= (int)sizeof(text))
		return;
	SceUID fd = ksceIoOpen("ur0:data/scebt-probe-status.txt",
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd >= 0) {
		write_all(fd, text, length);
		ksceIoClose(fd);
	}
}

static int dump_segment(const char *prefix, unsigned int index,
			const SceKernelSegmentInfo *segment)
{
	char path[64];
	snprintf(path, sizeof(path), "%s/%ssegment%u.bin", dump_dir, prefix, index);

	SceUID fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return fd;

	int ret = write_all(fd, segment->vaddr, segment->filesz);
	int close_ret = ksceIoClose(fd);
	return ret < 0 ? ret : close_ret;
}

static int append_module(char *text, int length, size_t capacity, const char *prefix,
			 const tai_module_info_t *tai, const SceKernelModuleInfo *module,
			 const int dump_results[4])
{
	length += snprintf(text + length, capacity - length,
		"%smodule=%s\n%smodid=0x%08X\n%smodule_nid=0x%08X\n"
		"%sexports=0x%08X-0x%08X\n%simports=0x%08X-0x%08X\n",
		prefix, tai->name, prefix, tai->modid, prefix, tai->module_nid, prefix,
		(unsigned int)tai->exports_start, (unsigned int)tai->exports_end,
		prefix,
		(unsigned int)tai->imports_start, (unsigned int)tai->imports_end);

	for (unsigned int i = 0; i < 4 && length > 0 && length < (int)capacity; i++) {
		const SceKernelSegmentInfo *s = &module->segments[i];
		length += snprintf(text + length, capacity - length,
			"%ssegment%u=vaddr:0x%08X,memsz:0x%08X,filesz:0x%08X,perms:0x%X,dump:%d\n",
			prefix, i, (unsigned int)s->vaddr, s->memsz, s->filesz,
			s->perms, dump_results[i]);
	}
	return length;
}

static int write_manifest(const tai_module_info_t *tai, const SceKernelModuleInfo *module,
			  const tai_module_info_t *wlanbt_tai,
			  const SceKernelModuleInfo *wlanbt_module, int wlanbt_result,
			  const SceKernelFwInfo *fw, const int dump_results[4],
			  const int wlanbt_dump_results[4])
{
	char text[2048];
	int length = snprintf(text, sizeof(text), "firmware=%s\ndump_dir=%s\n",
		fw->versionString, dump_dir);
	length = append_module(text, length, sizeof(text), "", tai, module, dump_results);
	length += snprintf(text + length, sizeof(text) - length,
		"wlanbt_lookup=%d\n", wlanbt_result);
	if (wlanbt_result >= 0)
		length = append_module(text, length, sizeof(text), "wlanbt_", wlanbt_tai,
				       wlanbt_module, wlanbt_dump_results);

	if (length <= 0 || length >= (int)sizeof(text))
		return -1;

	char path[64];
	snprintf(path, sizeof(path), "%s/manifest.txt", dump_dir);
	SceUID fd = ksceIoOpen(path,
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return fd;

	int ret = write_all(fd, text, length);
	int close_ret = ksceIoClose(fd);
	return ret < 0 ? ret : close_ret;
}

static int dump_loaded_modules(void)
{
	dump_dir = UX0_DUMP_DIR;
	if (prepare_dump_dir(dump_dir) < 0) {
		dump_dir = UR0_DUMP_DIR;
		if (prepare_dump_dir(dump_dir) < 0)
			return SCE_KERNEL_START_FAILED;
	}

	tai_module_info_t tai;
	memset(&tai, 0, sizeof(tai));
	tai.size = sizeof(tai);
	if (taiGetModuleInfoForKernel(KERNEL_PID, "SceBt", &tai) < 0)
		return SCE_KERNEL_START_FAILED;

	SceKernelModuleInfo module;
	memset(&module, 0, sizeof(module));
	module.size = sizeof(module);
	if (ksceKernelGetModuleInfo(KERNEL_PID, tai.modid, &module) < 0)
		return SCE_KERNEL_START_FAILED;

	SceKernelFwInfo fw;
	memset(&fw, 0, sizeof(fw));
	fw.size = sizeof(fw);
	if (ksceKernelGetSystemSwVersion(&fw) < 0)
		return SCE_KERNEL_START_FAILED;

	int results[4];
	for (unsigned int i = 0; i < 4; i++)
		results[i] = module.segments[i].vaddr && module.segments[i].filesz
			? dump_segment("", i, &module.segments[i]) : 0;

	tai_module_info_t wlanbt_tai;
	memset(&wlanbt_tai, 0, sizeof(wlanbt_tai));
	wlanbt_tai.size = sizeof(wlanbt_tai);
	SceKernelModuleInfo wlanbt_module;
	memset(&wlanbt_module, 0, sizeof(wlanbt_module));
	wlanbt_module.size = sizeof(wlanbt_module);
	int wlanbt_results[4] = {0};
	int wlanbt_result = taiGetModuleInfoForKernel(KERNEL_PID, "SceWlanBt", &wlanbt_tai);
	if (wlanbt_result >= 0) {
		wlanbt_result = ksceKernelGetModuleInfo(KERNEL_PID, wlanbt_tai.modid,
						       &wlanbt_module);
		if (wlanbt_result >= 0)
			for (unsigned int i = 0; i < 4; i++)
				wlanbt_results[i] = wlanbt_module.segments[i].vaddr &&
					wlanbt_module.segments[i].filesz
					? dump_segment("wlanbt-", i, &wlanbt_module.segments[i]) : 0;
	}

	return write_manifest(&tai, &module, &wlanbt_tai, &wlanbt_module,
			      wlanbt_result, &fw, results, wlanbt_results) < 0
		? SCE_KERNEL_START_FAILED : SCE_KERNEL_START_SUCCESS;
}

static int dump_worker(SceSize argc, void *args)
{
	(void)argc;
	(void)args;

	write_status("worker_started", 0);
	for (int i = 0; i < 120 && !stop_requested; i++) {
		tai_module_info_t tai;
		memset(&tai, 0, sizeof(tai));
		tai.size = sizeof(tai);
		if (taiGetModuleInfoForKernel(KERNEL_PID, "SceBt", &tai) >= 0) {
			write_status("scebt_found", tai.modid);
			int result = dump_loaded_modules();
			write_status("dump_finished", result);
			return result;
		}
		ksceKernelDelayThread(500000);
	}
	write_status("scebt_timeout", -1);
	return SCE_KERNEL_START_FAILED;
}

int module_start(SceSize argc, const void *args)
{
	(void)argc;
	(void)args;

	stop_requested = 0;
	write_status("module_start", 0);
	worker_uid = ksceKernelCreateThread("scebt_probe_worker", dump_worker,
		0x10000100, 0x4000, 0, 0, NULL);
	if (worker_uid < 0) {
		write_status("thread_create_failed", worker_uid);
		return SCE_KERNEL_START_FAILED;
	}
	int start_result = ksceKernelStartThread(worker_uid, 0, NULL);
	if (start_result < 0) {
		write_status("thread_start_failed", start_result);
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
		ksceKernelWaitThreadEnd(worker_uid, NULL, NULL);
		ksceKernelDeleteThread(worker_uid);
		worker_uid = -1;
	}
	return SCE_KERNEL_STOP_SUCCESS;
}
