#include "internal.h"

#include <stdio.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <sys/utsname.h>
#endif

/* Removes the spaces around `text`, in place. */
static char* Aeron_SystemTrim(char* text) {
	size_t length;

	while (*text == ' ' || *text == '\t') {
		++text;
	}
	length = SDL_strlen(text);
	while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\n' ||
						  text[length - 1] == '\r')) {
		text[--length] = '\0';
	}
	return text;
}

#ifdef _WIN32
/* Reads a string value under HKEY_LOCAL_MACHINE in UTF-8. Returns 0 when it is missing. */
static int Aeron_SystemRegistryString(const wchar_t* key, const wchar_t* name, char* out, size_t out_size) {
	wchar_t value[256];
	DWORD   size = sizeof value;

	if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, NULL, value, &size) != ERROR_SUCCESS ||
		WideCharToMultiByte(CP_UTF8, 0, value, -1, out, (int)out_size, NULL, NULL) == 0) {
		return 0;
	}
	return 1;
}

static void Aeron_SystemOs(char* out, size_t out_size) {
	typedef LONG(WINAPI * RtlGetVersionFunction)(OSVERSIONINFOW*);
	static const wchar_t  k_version_key[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
	OSVERSIONINFOW        info;
	HMODULE               ntdll       = GetModuleHandleW(L"ntdll.dll");
	RtlGetVersionFunction get_version = NULL;
	char                  release[64] = "";
	DWORD                 revision    = 0;
	DWORD                 size        = sizeof revision;

	SDL_zero(info);
	info.dwOSVersionInfoSize = sizeof info;
	/* GetVersionEx reports Windows 8 to programs whose manifest doesn't list
	 * later versions. RtlGetVersion reports the real version. */
	if (ntdll) {
		get_version = (RtlGetVersionFunction)(void*)GetProcAddress(ntdll, "RtlGetVersion");
	}
	if (!get_version || get_version(&info) != 0) {
		SDL_snprintf(out, out_size, "Windows");
		return;
	}
	Aeron_SystemRegistryString(k_version_key, L"DisplayVersion", release, sizeof release);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, k_version_key, L"UBR", RRF_RT_REG_DWORD, NULL, &revision, &size) !=
		ERROR_SUCCESS) {
		revision = 0;
	}
	/* Windows 11 still reports version 10.0, with builds from 22000. */
	SDL_snprintf(out, out_size, "Windows %s%s%s (%lu.%lu.%lu.%lu)",
				 info.dwMajorVersion == 10 && info.dwBuildNumber >= 22000 ? "11"
				 : info.dwMajorVersion == 10                              ? "10"
																		  : "",
				 release[0] ? " " : "", release, (unsigned long)info.dwMajorVersion,
				 (unsigned long)info.dwMinorVersion, (unsigned long)info.dwBuildNumber,
				 (unsigned long)revision);
}

static void Aeron_SystemCpu(char* out, size_t out_size) {
	char name[256];

	if (Aeron_SystemRegistryString(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
								   L"ProcessorNameString", name, sizeof name)) {
		SDL_snprintf(out, out_size, "%s", Aeron_SystemTrim(name));
	}
}
#elif defined(__APPLE__)
/* Reads a string from sysctl. Returns 0 when it is missing. */
static int Aeron_SystemSysctl(const char* name, char* out, size_t out_size) {
	size_t size = out_size;

	if (sysctlbyname(name, out, &size, NULL, 0) != 0 || size == 0) {
		out[0] = '\0';
		return 0;
	}
	out[out_size - 1] = '\0';
	return 1;
}

static void Aeron_SystemOs(char* out, size_t out_size) {
	char version[64];
	char build[64];
	char model[64];

	if (!Aeron_SystemSysctl("kern.osproductversion", version, sizeof version)) {
		SDL_snprintf(out, out_size, "macOS");
		return;
	}
	Aeron_SystemSysctl("kern.osversion", build, sizeof build);
	Aeron_SystemSysctl("hw.model", model, sizeof model);
	SDL_snprintf(out, out_size, "macOS %s%s%s%s%s%s", version, build[0] ? " (" : "", build,
				 build[0] ? ")" : "", model[0] ? ", " : "", model);
}

static void Aeron_SystemCpu(char* out, size_t out_size) {
	char name[256];

	if (Aeron_SystemSysctl("machdep.cpu.brand_string", name, sizeof name)) {
		SDL_snprintf(out, out_size, "%s", Aeron_SystemTrim(name));
	}
}
#else
/* Finds the line of `path` that starts with `prefix` and copies the rest of
 * it. Returns 0 when there is none. */
static int Aeron_SystemFileLine(const char* path, const char* prefix, char* out, size_t out_size) {
	char  line[512];
	FILE* file  = fopen(path, "r");
	int   found = 0;

	if (!file) {
		return 0;
	}
	while (!found && fgets(line, sizeof line, file)) {
		if (SDL_strncmp(line, prefix, SDL_strlen(prefix)) == 0) {
			SDL_snprintf(out, out_size, "%s", Aeron_SystemTrim(line + SDL_strlen(prefix)));
			found = 1;
		}
	}
	fclose(file);
	return found;
}

static void Aeron_SystemOs(char* out, size_t out_size) {
	char           distribution[256] = "";
	char*          name              = distribution;
	struct utsname system;

	if (Aeron_SystemFileLine("/etc/os-release", "PRETTY_NAME=", distribution, sizeof distribution)) {
		size_t length = SDL_strlen(name);
		if (length >= 2 && (name[0] == '"' || name[0] == '\'') && name[length - 1] == name[0]) {
			name[length - 1] = '\0';
			++name;
		}
	}
	if (uname(&system) != 0) {
		SDL_snprintf(out, out_size, "%s", name[0] ? name : SDL_GetPlatform());
		return;
	}
	SDL_snprintf(out, out_size, "%s%s%s %s %s", name, name[0] ? ", " : "", system.sysname, system.release,
				 system.machine);
}

static void Aeron_SystemCpu(char* out, size_t out_size) {
	char name[256];

	/* x86 names the processor in "model name", some ARM kernels in "Hardware". */
	if (Aeron_SystemFileLine("/proc/cpuinfo", "model name", name, sizeof name) ||
		Aeron_SystemFileLine("/proc/cpuinfo", "Hardware", name, sizeof name)) {
		const char* colon = SDL_strchr(name, ':');
		SDL_snprintf(out, out_size, "%s", colon ? Aeron_SystemTrim((char*)colon + 1) : name);
	}
}
#endif

void Aeron_SystemDescribeOs(char* out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	out[0] = '\0';
	Aeron_SystemOs(out, out_size);
}

void Aeron_LogSystemInfo(void) {
	char         cpu[256] = "";
	char         locales[128];
	size_t       length       = 0;
	int          locale_count = 0;
	SDL_Locale** preferred    = SDL_GetPreferredLocales(&locale_count);

	Aeron_SystemCpu(cpu, sizeof cpu);
	locales[0] = '\0';
	for (int index = 0; preferred && index < locale_count && index < 4 && length < sizeof locales; ++index) {
		int written = SDL_snprintf(locales + length, sizeof locales - length, "%s%s%s%s", index ? ", " : "",
								   preferred[index]->language, preferred[index]->country ? "_" : "",
								   preferred[index]->country ? preferred[index]->country : "");
		if (written < 0) {
			break;
		}
		length += (size_t)written;
	}
	SDL_free(preferred);
#ifdef _WIN32
	Aeron_LogInfo("aeron", "CPU %s, %d logical cores, %.1f GB RAM; locales %s; code page %u",
				  cpu[0] ? cpu : "unknown", SDL_GetNumLogicalCPUCores(), SDL_GetSystemRAM() / 1024.0,
				  locales[0] ? locales : "unknown", (unsigned)GetACP());
#else
	Aeron_LogInfo("aeron", "CPU %s, %d logical cores, %.1f GB RAM; locales %s", cpu[0] ? cpu : "unknown",
				  SDL_GetNumLogicalCPUCores(), SDL_GetSystemRAM() / 1024.0, locales[0] ? locales : "unknown");
#endif
}
