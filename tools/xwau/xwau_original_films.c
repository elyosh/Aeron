#include "xwau_original_films.h"

#include "util.h"
#include "xwau_config.h"

enum {
	STOCK_CRAFT_SIZE = 1017,
	STOCK_MESHES = 50,
	MAX_MESHES = 255,
	HOOK_ENGINES = 255,
	HOOK_RACKS = 128,
	RACK_SIZE = 14,
};

bool XwauOriginalFilms_Write(FILE* f, const char* game) {
	/* The builds of the hook that read Craft_ExtraSize. The layout of older
	   builds isn't known. */
	if (!Xwau_DllHasText(game, "hook_opt_limit.dll", "Craft_ExtraSize"))
		return false;
	XwauSection settings;
	if (!XwauSection_ReadHook(game, "hook_opt_limit", &settings))
		return false;
	const char* const source = settings.source[0] ? settings.source : "hook_opt_limit.cfg";
	const long long   meshes = XwauSection_IntOr(&settings, "MeshesCount", MAX_MESHES);
	if (meshes == 0) {
		XwauSection_Free(&settings);
		return false;
	}
	/* The hook's offsets are from the end of the stock record. */
	const long long size = STOCK_CRAFT_SIZE + XwauSection_IntOr(&settings, "Craft_ExtraSize",
																 meshes * 3 + HOOK_ENGINES + HOOK_RACKS * RACK_SIZE);
	const long long state     = STOCK_CRAFT_SIZE + XwauSection_IntOr(&settings, "Craft_Offset_22E", 0);
	const long long rotation  = STOCK_CRAFT_SIZE + XwauSection_IntOr(&settings, "Craft_Offset_260", meshes);
	const long long hp        = STOCK_CRAFT_SIZE + XwauSection_IntOr(&settings, "Craft_Offset_292", meshes * 2);
	const long long engine_health   = STOCK_CRAFT_SIZE + XwauSection_IntOr(&settings, "Craft_Offset_2CF", meshes * 3);
	const long long rack_data     = STOCK_CRAFT_SIZE + XwauSection_IntOr(&settings, "Craft_Offset_2DF",
																	 meshes * 3 + HOOK_ENGINES);
	XwauSection_Free(&settings);
	if (meshes < STOCK_MESHES || meshes > MAX_MESHES) {
		Report(source, "MeshesCount = %lld: films with this many meshes can't be played; the film settings are "
					   "left out",
			   meshes);
		return false;
	}
	if (state < STOCK_CRAFT_SIZE || rotation < STOCK_CRAFT_SIZE || hp < STOCK_CRAFT_SIZE ||
		engine_health < STOCK_CRAFT_SIZE || rack_data < STOCK_CRAFT_SIZE || state + meshes > size ||
		rotation + meshes > size || hp + meshes > size || engine_health + HOOK_ENGINES > size ||
		rack_data + HOOK_RACKS * RACK_SIZE > size || size > 65535) {
		Report(source, "the craft offsets don't fit Craft_ExtraSize; the film settings are left out");
		return false;
	}
	/* The counts of the moved arrays: the hook always has 255 engines, and
	   its default Craft_ExtraSize leaves room for 128 weapon racks. */
	fprintf(f,
			"original_films:\n"
			"  craft_record_size: %lld\n"
			"  meshes: %lld\n"
			"  engines: %d\n"
			"  weapon_racks: %d\n"
			"  component_state: %lld\n"
			"  mesh_rotation: %lld\n"
			"  component_hp: %lld\n"
			"  engine_health: %lld\n"
			"  weapon_rack_data: %lld\n",
			size, meshes, HOOK_ENGINES, HOOK_RACKS, state, rotation, hp, engine_health, rack_data);
	return true;
}
