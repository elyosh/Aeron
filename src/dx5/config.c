#include "aeron/dx5/compat.h"

#include "internal.h"

#include <string.h>

static AeronDx5Config g_dx5Config;

void AeronDx5_Configure(const AeronDx5Config* config) {
	if (config)
		g_dx5Config = *config;
	else
		memset(&g_dx5Config, 0, sizeof(g_dx5Config));
}

AeronRectI AeronDx5_PresentationRect(int surface_width, int surface_height) {
	if (g_dx5Config.presentation_rect) {
		const AeronDx5Rect rect =
			g_dx5Config.presentation_rect(g_dx5Config.context, surface_width, surface_height);
		return (AeronRectI) { rect.x, rect.y, rect.width, rect.height };
	}
	return (AeronRectI) { 0, 0, surface_width, surface_height };
}

void AeronDx5_NotifyPresent(int surface_width, int surface_height) {
	if (g_dx5Config.presented)
		g_dx5Config.presented(g_dx5Config.context, surface_width, surface_height);
}

void AeronDx5_Shutdown(void) {
	D3DCompat_Shutdown();
	AeronDx5_ResetPresentationState();
	memset(&g_dx5Config, 0, sizeof(g_dx5Config));
}
