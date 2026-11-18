/* Reads the head's pose through the TrackIR interface: NPClient64.dll, which
 * the TrackIR software installs. OpenTrack's "freetrack 2.0 Enhanced" output
 * installs a DLL with the same name and functions, so it is read here too.
 * The registry says where the DLL is. Neither product is linked or shipped
 * with Aeron. */

#include "head_tracker_internal.h"

#include "aeron/log.h"

#include <string.h>
#include <wchar.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* The ID that the program gives the tracker's software, which lists the
 * games it knows. Aeron has no ID of its own. FreePIE and other free
 * programs use this one. OpenTrack lists it as a game whose data isn't
 * encrypted. */
#define TRACKIR_PROGRAM_ID 13302
/* What NP_RequestData asks for: roll 1, pitch 2, yaw 4, X 16, Y 32, Z 64. */
#define TRACKIR_DATA_FIELDS 0x77
#define TRACKIR_STATUS_ACTIVE 0
#define TRACKIR_RETRY_US 2000000u

#pragma pack(push, 1)

typedef struct TrackIRFrame {
	uint16_t status;
	/* Changes when the frame is new. */
	uint16_t signature;
	uint32_t io_data;
	float    roll, pitch, yaw;
	float    x, y, z;
	float    raw[3], delta[3], smooth[3];
} TrackIRFrame;

#pragma pack(pop)

typedef int(__stdcall* TrackIRCall)(void);
typedef int(__stdcall* TrackIRRegisterWindow)(HWND window);
typedef int(__stdcall* TrackIRSendWord)(unsigned short value);
typedef int(__stdcall* TrackIRGetData)(TrackIRFrame* frame);

static struct {
	HMODULE               module;
	TrackIRRegisterWindow register_window;
	TrackIRCall           unregister_window;
	TrackIRSendWord       register_program;
	TrackIRSendWord       request_data;
	TrackIRCall           start_data;
	TrackIRCall           stop_data;
	TrackIRGetData        get_data;
	/* The DLL may not have these two. */
	TrackIRCall stop_cursor;
	TrackIRCall start_cursor;
	int         registered;
	int         attempted;
	uint64_t    next_attempt_us;
	int         has_signature;
	uint16_t    signature;
	/* The reason for the last failed connection, so that it is logged once
	 * and not at every retry. */
	const char* logged_problem;
} g_trackir;

/* A function of the DLL, or NULL. The cast goes through a generic function
 * pointer, because GetProcAddress returns one type for every function. */
static void (*trackir_function(const char* name))(void) {
	return (void (*)(void))GetProcAddress(g_trackir.module, name);
}

/* Finds NPClient64.dll and loads it. Returns the reason when it can't, and
 * NULL when the DLL is loaded. */
static const char* trackir_load(void) {
	wchar_t path[MAX_PATH + 16];
	DWORD   size = (DWORD)(MAX_PATH * sizeof(wchar_t));
	if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\NaturalPoint\\NATURALPOINT\\NPClient Location", L"Path",
					 RRF_RT_REG_SZ, NULL, path, &size) != ERROR_SUCCESS ||
		path[0] == L'\0')
		return "NPClient64.dll was not found: the registry has no path to it";
	size_t length = wcslen(path);
	/* OpenTrack writes the path with forward slashes, which LoadLibrary
	 * doesn't accept in a path. */
	for (size_t i = 0; i < length; ++i) {
		if (path[i] == L'/')
			path[i] = L'\\';
	}
	if (path[length - 1] != L'\\')
		path[length++] = L'\\';
	wcscpy(path + length, L"NPClient64.dll");
	g_trackir.module = LoadLibraryW(path);
	if (!g_trackir.module)
		return "NPClient64.dll could not be loaded";

	g_trackir.register_window   = (TrackIRRegisterWindow)trackir_function("NP_RegisterWindowHandle");
	g_trackir.unregister_window = (TrackIRCall)trackir_function("NP_UnregisterWindowHandle");
	g_trackir.register_program  = (TrackIRSendWord)trackir_function("NP_RegisterProgramProfileID");
	g_trackir.request_data      = (TrackIRSendWord)trackir_function("NP_RequestData");
	g_trackir.start_data        = (TrackIRCall)trackir_function("NP_StartDataTransmission");
	g_trackir.stop_data         = (TrackIRCall)trackir_function("NP_StopDataTransmission");
	g_trackir.get_data          = (TrackIRGetData)trackir_function("NP_GetData");
	g_trackir.stop_cursor       = (TrackIRCall)trackir_function("NP_StopCursor");
	g_trackir.start_cursor      = (TrackIRCall)trackir_function("NP_StartCursor");
	if (!g_trackir.register_window || !g_trackir.unregister_window || !g_trackir.register_program ||
		!g_trackir.request_data || !g_trackir.start_data || !g_trackir.stop_data || !g_trackir.get_data) {
		FreeLibrary(g_trackir.module);
		g_trackir.module = NULL;
		return "NPClient64.dll doesn't have the functions of the TrackIR interface";
	}
	return NULL;
}

/* Registers the window and the program, and starts the data. Returns the
 * reason when a call fails. */
static const char* trackir_register(HWND window) {
	if (g_trackir.register_window(window) != 0)
		return "the tracker's software refused the window: it may not be running";
	if (g_trackir.register_program(TRACKIR_PROGRAM_ID) != 0) {
		g_trackir.unregister_window();
		return "the tracker's software refused the program ID";
	}
	if (g_trackir.request_data(TRACKIR_DATA_FIELDS) != 0) {
		g_trackir.unregister_window();
		return "the tracker's software refused the data request";
	}
	/* The tracker's software can move the mouse with the head. It must not
	 * do that while the application uses the head pose. */
	if (g_trackir.stop_cursor)
		g_trackir.stop_cursor();
	if (g_trackir.start_data() != 0) {
		if (g_trackir.start_cursor)
			g_trackir.start_cursor();
		g_trackir.unregister_window();
		return "the tracker's software didn't start sending";
	}
	return NULL;
}

static int trackir_connect(HWND window, uint64_t now_us) {
	if (g_trackir.attempted && now_us < g_trackir.next_attempt_us)
		return 0;
	g_trackir.attempted       = 1;
	g_trackir.next_attempt_us = now_us + TRACKIR_RETRY_US;
	const char* problem       = g_trackir.module ? NULL : trackir_load();
	if (!problem)
		problem = trackir_register(window);
	if (problem) {
		/* Each reason is a string literal, so the pointers can be compared. */
		if (problem != g_trackir.logged_problem)
			Aeron_LogInfo("aeron.head", "TrackIR: %s", problem);
		g_trackir.logged_problem = problem;
		return 0;
	}
	Aeron_LogInfo("aeron.head", "TrackIR: connected");
	g_trackir.logged_problem = NULL;
	g_trackir.registered     = 1;
	g_trackir.has_signature  = 0;
	return 1;
}

int AeronHeadTrackIR_Read(void* window, uint64_t now_us, float values[6]) {
	if (!window)
		return 0;
	if (!g_trackir.registered && !trackir_connect((HWND)window, now_us))
		return 0;
	TrackIRFrame frame;
	memset(&frame, 0, sizeof frame);
	/* OpenTrack's DLL returns an error while the head is exactly at the
	 * centre, and before OpenTrack starts. So an error only means that
	 * this frame has no data. */
	if (g_trackir.get_data(&frame) != 0 || frame.status != TRACKIR_STATUS_ACTIVE)
		return 0;
	/* The TrackIR software keeps the signature while it has no new frame.
	 * OpenTrack's DLL changes it at every call. */
	if (g_trackir.has_signature && frame.signature == g_trackir.signature)
		return 0;
	g_trackir.has_signature = 1;
	g_trackir.signature     = frame.signature;
	values[0]               = frame.roll;
	values[1]               = frame.pitch;
	values[2]               = frame.yaw;
	values[3]               = frame.x;
	values[4]               = frame.y;
	values[5]               = frame.z;
	return 1;
}

void AeronHeadTrackIR_Close(void) {
	if (g_trackir.registered) {
		g_trackir.stop_data();
		if (g_trackir.start_cursor)
			g_trackir.start_cursor();
		g_trackir.unregister_window();
	}
	if (g_trackir.module)
		FreeLibrary(g_trackir.module);
	memset(&g_trackir, 0, sizeof g_trackir);
}
