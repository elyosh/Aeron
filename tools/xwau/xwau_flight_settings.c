#include "xwau_flight_settings.h"

#include <stdio.h>

#include "util.h"
#include "xwau_config.h"
#include "xwau_mission_settings.h"

bool XwauFlightSettings_Write(const char* game, const char* path) {
	char               found[1024];
	const bool         extra_planets = Xwau_FindFile(game, "hook_backdrops.dll", found, sizeof found);
	XwauSettingsConfig config;
	if (!XwauSettingsConfig_Read(game, &config)) {
		XwauSettingsConfig_Free(&config);
		return false;
	}
	FILE* f = fopen(path, "wb");
	if (!f) {
		Report(NULL, "cannot write %s", path);
		XwauSettingsConfig_Free(&config);
		return false;
	}
	fputs("# Made by xwau_import missions.\nversion: 1\n", f);
	if (extra_planets)
		fputs("backdrops:\n  extra_planets:\n    - { ids: [104, 255], first_group: 6304 }\n", f);
	const bool settings = XwauMissionSettings_WriteDefaults(f, &config);
	XwauSettingsConfig_Free(&config);
	const bool ok = !ferror(f) && fclose(f) == 0;
	if (!ok)
		Report(NULL, "cannot write %s", path);
	else
		Report(NULL, "wrote %s: %s%s%s", path, extra_planets ? "backdrop ids 104 to 255 are planets" : "",
			   extra_planets && settings ? ", " : "", settings ? "the mission rules and defaults" : "");
	if (ok && !extra_planets && !settings)
		Report(NULL, "%s has no flight settings", path);
	return ok;
}
