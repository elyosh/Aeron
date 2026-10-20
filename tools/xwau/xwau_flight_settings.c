#include "xwau_flight_settings.h"

#include <stdio.h>

#include "util.h"
#include "xwau_config.h"

bool XwauFlightSettings_Write(const char* game, const char* path) {
	char       found[1024];
	const bool extra_planets = Xwau_FindFile(game, "hook_backdrops.dll", found, sizeof found);
	FILE*      f             = fopen(path, "wb");
	if (!f) {
		Report(NULL, "cannot write %s", path);
		return false;
	}
	fputs("# Made by xwau_import missions.\nversion: 1\n", f);
	if (extra_planets)
		fputs("backdrops:\n  extra_planets:\n    - { ids: [104, 255], first_group: 6304 }\n", f);
	const bool ok = !ferror(f) && fclose(f) == 0;
	if (!ok)
		Report(NULL, "cannot write %s", path);
	else
		Report(NULL, "wrote %s: %s", path,
			   extra_planets ? "backdrop ids 104 to 255 are planets" : "no flight settings");
	return ok;
}
