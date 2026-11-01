#include "internal.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <dbghelp.h>
#include <shellapi.h>
#include <signal.h>
#include <stdio.h>

/*
 * Process-wide crash reporting. A reporter thread is created up front and
 * parked, so the faulting thread only has to hand over its exception
 * pointers: it may have no stack left (overflow) or hold locks the report
 * would need. The reporter writes a minidump, appends the exception and an
 * unwound call stack to the session log, then tells the player where the
 * files are before the process is terminated.
 */

#define AERON_CRASH_ABORT_EXCEPTION 0xE0000A01u
#define AERON_STATUS_HEAP_CORRUPTION 0xC0000374u
#define AERON_STATUS_STACK_BUFFER_OVERRUN 0xC0000409u
#define AERON_CRASH_STACK_FRAMES 48

typedef BOOL(WINAPI* AeronMiniDumpWriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
											 PMINIDUMP_EXCEPTION_INFORMATION,
											 PMINIDUMP_USER_STREAM_INFORMATION,
											 PMINIDUMP_CALLBACK_INFORMATION);

static struct {
	AeronMiniDumpWriteDump write_dump;
	HMODULE                executable;
	HANDLE                 request_event;
	HANDLE                 reporter_thread;
	DWORD                  reporter_thread_id;
	volatile LONG          crashing;
	EXCEPTION_POINTERS*    exception;
	DWORD                  thread_id;
	ULONG_PTR              stack_low;
	ULONG_PTR              stack_high;
	wchar_t                directory[MAX_PATH];
	wchar_t                dump_path[MAX_PATH];
	wchar_t                app_name[64];
	/* UTF-8 copies for the log: SDL's %ls conversion allocates. */
	char directory_utf8[AERON_MAX_PATH];
	char dump_path_utf8[AERON_MAX_PATH];
	char app_name_utf8[64];
	char app_version[64];
} s_crash;

static void Aeron_CrashLog(const char* fmt, ...) {
	char    line[1024];
	HANDLE  log_file;
	DWORD   written;
	va_list args;
	int     length;

	va_start(args, fmt);
	length = SDL_vsnprintf(line, sizeof line - 1, fmt, args);
	va_end(args);
	if (length < 0) {
		return;
	}
	if (length > (int)sizeof line - 2) {
		length = (int)sizeof line - 2;
	}
	line[length++] = '\n';
	line[length]   = '\0';
	OutputDebugStringA(line);
	/* SDL's log lock may be held by the faulting thread, so write to the handle directly. */
	log_file = (HANDLE)Aeron_LogFileNativeHandle();
	if (log_file && log_file != INVALID_HANDLE_VALUE) {
		WriteFile(log_file, line, (DWORD)length, &written, NULL);
		FlushFileBuffers(log_file);
	}
}

static const char* Aeron_CrashExceptionName(DWORD code) {
	switch (code) {
		case EXCEPTION_ACCESS_VIOLATION:
			return "access violation";
		case EXCEPTION_STACK_OVERFLOW:
			return "stack overflow";
		case EXCEPTION_ILLEGAL_INSTRUCTION:
			return "illegal instruction";
		case EXCEPTION_PRIV_INSTRUCTION:
			return "privileged instruction";
		case EXCEPTION_INT_DIVIDE_BY_ZERO:
			return "integer divide by zero";
		case EXCEPTION_INT_OVERFLOW:
			return "integer overflow";
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
			return "array bounds exceeded";
		case EXCEPTION_DATATYPE_MISALIGNMENT:
			return "datatype misalignment";
		case EXCEPTION_IN_PAGE_ERROR:
			return "in-page error";
		case EXCEPTION_BREAKPOINT:
			return "breakpoint";
		case EXCEPTION_NONCONTINUABLE_EXCEPTION:
			return "noncontinuable exception";
		case AERON_STATUS_HEAP_CORRUPTION:
			return "heap corruption";
		case AERON_STATUS_STACK_BUFFER_OVERRUN:
			return "stack buffer overrun";
		case AERON_CRASH_ABORT_EXCEPTION:
			return "abort()";
		default:
			return "unhandled exception";
	}
}

/* Formats an address as module+offset; module-relative offsets survive ASLR. */
static void Aeron_CrashDescribeAddress(DWORD64 address, char* out, size_t capacity) {
	HMODULE     module = NULL;
	char        path[MAX_PATH];
	const char* name;
	DWORD       length;

	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
							   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						   (LPCSTR)(ULONG_PTR)address, &module) &&
		(length = GetModuleFileNameA(module, path, sizeof path)) > 0 && length < sizeof path) {
		name = SDL_strrchr(path, '\\');
		name = name ? name + 1 : path;
		SDL_snprintf(out, capacity, "0x%016llx %s+0x%llx", (unsigned long long)address, name,
					 (unsigned long long)(address - (DWORD64)(ULONG_PTR)module));
	} else {
		SDL_snprintf(out, capacity, "0x%016llx", (unsigned long long)address);
	}
}

static int Aeron_CrashOnStack(DWORD64 address) {
	return address >= s_crash.stack_low && address + sizeof(DWORD64) <= s_crash.stack_high;
}

static void Aeron_CrashLogStack(const CONTEXT* crash_context) {
#if defined(_M_X64) || defined(__x86_64__)
	CONTEXT context = *crash_context;
	char    where[MAX_PATH + 64];

	for (int frame = 0; frame < AERON_CRASH_STACK_FRAMES; ++frame) {
		DWORD64           image_base;
		PRUNTIME_FUNCTION function;
		PVOID             handler_data;
		DWORD64           establisher_frame;

		Aeron_CrashDescribeAddress(context.Rip, where, sizeof where);
		Aeron_CrashLog("crash:   #%02d %s", frame, where);
		if (!Aeron_CrashOnStack(context.Rsp)) {
			break;
		}
		function = RtlLookupFunctionEntry(context.Rip, &image_base, NULL);
		if (function) {
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
							 &establisher_frame, NULL);
		} else {
			/* Leaf function or a call through a bad pointer: the return address is at RSP. */
			context.Rip = *(const DWORD64*)(ULONG_PTR)context.Rsp;
			context.Rsp += sizeof(DWORD64);
		}
		if (!context.Rip) {
			break;
		}
	}
#else
	(void)crash_context;
#endif
}

static BOOL CALLBACK Aeron_CrashDumpCallback(PVOID param, const PMINIDUMP_CALLBACK_INPUT input,
											 PMINIDUMP_CALLBACK_OUTPUT output) {
	(void)param;
	if (!input || !output) {
		return FALSE;
	}
	switch (input->CallbackType) {
		case ModuleCallback:
			/* Keep globals for the executable, where the game state lives, but not
			 * for system and driver DLLs, which would bloat the dump. */
			if ((HMODULE)(ULONG_PTR)input->Module.BaseOfImage != s_crash.executable) {
				output->ModuleWriteFlags &= ~ModuleWriteDataSeg;
			}
			return TRUE;
		case IncludeModuleCallback:
		case IncludeThreadCallback:
		case ThreadCallback:
		case ThreadExCallback:
			return TRUE;
		default:
			return FALSE;
	}
}

static int Aeron_CrashWriteDump(void) {
	MINIDUMP_EXCEPTION_INFORMATION dump_exception;
	MINIDUMP_CALLBACK_INFORMATION  callback_info;
	SYSTEMTIME                     now;
	HANDLE                         file;
	BOOL                           written;

	if (!s_crash.write_dump) {
		Aeron_CrashLog("crash: dbghelp.dll is unavailable; no minidump written");
		return 0;
	}
	GetLocalTime(&now);
	SDL_snprintf(s_crash.dump_path_utf8, sizeof s_crash.dump_path_utf8,
				 "%s\\%s-%04u%02u%02u-%02u%02u%02u.dmp", s_crash.directory_utf8, s_crash.app_name_utf8,
				 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
	if (!MultiByteToWideChar(CP_UTF8, 0, s_crash.dump_path_utf8, -1, s_crash.dump_path, MAX_PATH)) {
		Aeron_CrashLog("crash: minidump path is too long");
		return 0;
	}
	file = CreateFileW(s_crash.dump_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		Aeron_CrashLog("crash: cannot create minidump file (error %lu)", GetLastError());
		return 0;
	}
	dump_exception.ThreadId          = s_crash.thread_id;
	dump_exception.ExceptionPointers = s_crash.exception;
	dump_exception.ClientPointers    = FALSE;
	callback_info.CallbackRoutine    = Aeron_CrashDumpCallback;
	callback_info.CallbackParam      = NULL;
	written                          = s_crash.write_dump(
		GetCurrentProcess(), GetCurrentProcessId(), file,
		(MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory |
						MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData),
		&dump_exception, NULL, &callback_info);
	CloseHandle(file);
	if (!written) {
		Aeron_CrashLog("crash: MiniDumpWriteDump failed (error 0x%08lx)", GetLastError());
		DeleteFileW(s_crash.dump_path);
		return 0;
	}
	Aeron_CrashLog("crash: minidump written to %s", s_crash.dump_path_utf8);
	return 1;
}

static void Aeron_CrashReport(void) {
	const EXCEPTION_RECORD* record = s_crash.exception->ExceptionRecord;
	wchar_t                 message[MAX_PATH * 2 + 256];
	wchar_t                 folder_argument[MAX_PATH + 16];
	char                    where[MAX_PATH + 64];
	int                     dumped;

	/* Release a relative-mouse cursor clip so the player can use the dialog. */
	ClipCursor(NULL);
	Aeron_CrashDescribeAddress((DWORD64)(ULONG_PTR)record->ExceptionAddress, where, sizeof where);
	Aeron_CrashLog("crash: %s %s: %s (0x%08lx) at %s on thread %08lx", s_crash.app_name_utf8,
				   s_crash.app_version, Aeron_CrashExceptionName(record->ExceptionCode),
				   record->ExceptionCode, where, s_crash.thread_id);
	if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
		Aeron_CrashLog("crash: %s address 0x%016llx",
					   record->ExceptionInformation[0] == 0   ? "reading"
					   : record->ExceptionInformation[0] == 8 ? "executing"
															  : "writing",
					   (unsigned long long)record->ExceptionInformation[1]);
	}
	dumped = Aeron_CrashWriteDump();
	Aeron_CrashLog("crash: call stack:");
	Aeron_CrashLogStack(s_crash.exception->ContextRecord);

	if (dumped) {
		_snwprintf(message, sizeof message / sizeof message[0],
				   L"%ls has crashed.\n\nA crash report was saved to:\n%ls\n\n"
				   L"Please zip and send the whole \"diagnostics\" folder when reporting the problem.\n\n"
				   L"Open the folder now?",
				   s_crash.app_name, s_crash.dump_path);
	} else {
		_snwprintf(message, sizeof message / sizeof message[0],
				   L"%ls has crashed.\n\nDetails were written to the log files in:\n%ls\n\n"
				   L"Please zip and send the whole \"diagnostics\" folder when reporting the problem.\n\n"
				   L"Open the folder now?",
				   s_crash.app_name, s_crash.directory);
	}
	message[sizeof message / sizeof message[0] - 1] = L'\0';
	if (MessageBoxW(NULL, message, s_crash.app_name,
					MB_YESNO | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND) == IDYES) {
		if (dumped) {
			_snwprintf(folder_argument, sizeof folder_argument / sizeof folder_argument[0],
					   L"/select,\"%ls\"", s_crash.dump_path);
		} else {
			_snwprintf(folder_argument, sizeof folder_argument / sizeof folder_argument[0], L"\"%ls\"",
					   s_crash.directory);
		}
		folder_argument[sizeof folder_argument / sizeof folder_argument[0] - 1] = L'\0';
		ShellExecuteW(NULL, L"open", L"explorer.exe", folder_argument, NULL, SW_SHOWNORMAL);
	}
}

static DWORD WINAPI Aeron_CrashReporterThread(LPVOID unused) {
	(void)unused;
	WaitForSingleObject(s_crash.request_event, INFINITE);
	Aeron_CrashReport();
	return 0;
}

static LONG WINAPI Aeron_CrashFilter(EXCEPTION_POINTERS* exception) {
	if (InterlockedCompareExchange(&s_crash.crashing, 1, 0) != 0) {
		/* The report itself faulted: give up on it. Any other thread waits
		 * for the reporter, which ends the process. */
		if (GetCurrentThreadId() ==
			(s_crash.reporter_thread ? s_crash.reporter_thread_id : s_crash.thread_id)) {
			TerminateProcess(GetCurrentProcess(), exception->ExceptionRecord->ExceptionCode);
		}
		Sleep(INFINITE);
	}
	s_crash.exception = exception;
	s_crash.thread_id = GetCurrentThreadId();
	GetCurrentThreadStackLimits(&s_crash.stack_low, &s_crash.stack_high);
	if (s_crash.reporter_thread) {
		SetEvent(s_crash.request_event);
		WaitForSingleObject(s_crash.reporter_thread, INFINITE);
	} else {
		Aeron_CrashReport();
	}
	/* Skip DLL detach and atexit handlers, which can deadlock on the locks the crash left held. */
	TerminateProcess(GetCurrentProcess(), exception->ExceptionRecord->ExceptionCode);
	return EXCEPTION_EXECUTE_HANDLER;
}

/* Heap corruption and /GS failures are fatal and may never reach the unhandled-exception filter. */
static LONG WINAPI Aeron_CrashVectoredHandler(EXCEPTION_POINTERS* exception) {
	DWORD code = exception->ExceptionRecord->ExceptionCode;
	if (code == AERON_STATUS_HEAP_CORRUPTION || code == AERON_STATUS_STACK_BUFFER_OVERRUN) {
		return Aeron_CrashFilter(exception);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

/* abort() and failed asserts exit without an exception; raise one so they get a dump with context. */
static void Aeron_CrashAbortSignal(int signal_number) {
	(void)signal_number;
	RaiseException(AERON_CRASH_ABORT_EXCEPTION, EXCEPTION_NONCONTINUABLE, 0, NULL);
}

void Aeron_CrashHandlerInit(const char* directory, const char* app_name, const char* app_version) {
	HMODULE dbghelp;
	ULONG   stack_guarantee = 64 * 1024;

	if (s_crash.request_event || !directory || !app_name) {
		return;
	}
	if (!MultiByteToWideChar(CP_UTF8, 0, directory, -1, s_crash.directory, MAX_PATH) ||
		!MultiByteToWideChar(CP_UTF8, 0, app_name, -1, s_crash.app_name,
							 sizeof s_crash.app_name / sizeof s_crash.app_name[0])) {
		Aeron_LogWarn("aeron.crash", "crash report path is too long; crash reporting disabled");
		return;
	}
	Aeron_CopyString(s_crash.directory_utf8, sizeof s_crash.directory_utf8, directory);
	Aeron_CopyString(s_crash.app_name_utf8, sizeof s_crash.app_name_utf8, app_name);
	Aeron_CopyString(s_crash.app_version, sizeof s_crash.app_version, app_version);
	if (!CreateDirectoryW(s_crash.directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
		Aeron_LogWarn("aeron.crash", "cannot create %s (error %lu); crash reporting disabled", directory,
					  GetLastError());
		return;
	}
	/* Loaded now rather than at crash time, when the loader lock may be held. */
	dbghelp = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (dbghelp) {
		s_crash.write_dump =
			(AeronMiniDumpWriteDump)(void (*)(void))GetProcAddress(dbghelp, "MiniDumpWriteDump");
	}
	if (!s_crash.write_dump) {
		Aeron_LogWarn("aeron.crash", "dbghelp.dll is unavailable; crashes will be logged without a minidump");
	}
	s_crash.executable    = GetModuleHandleW(NULL);
	s_crash.request_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (s_crash.request_event) {
		s_crash.reporter_thread =
			CreateThread(NULL, 0, Aeron_CrashReporterThread, NULL, 0, &s_crash.reporter_thread_id);
	}
	/* Leaves the main thread room to run the filter after a stack overflow. */
	SetThreadStackGuarantee(&stack_guarantee);
	SetUnhandledExceptionFilter(Aeron_CrashFilter);
	AddVectoredExceptionHandler(0, Aeron_CrashVectoredHandler);
	signal(SIGABRT, Aeron_CrashAbortSignal);
	Aeron_LogInfo("aeron.crash", "crash reports will be written to %s", directory);
}
