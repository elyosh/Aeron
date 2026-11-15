#include "xwau_exe_patches.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "util.h"
#include "xwau_game_tables.h"

enum { MAX_PARTS = 2, MAX_BYTES = 8 };

/* The stock and patched bytes at one address. */
typedef struct Part {
	uint32_t address;
	size_t   size;
	uint8_t  stock[MAX_BYTES];
	uint8_t  patched[MAX_BYTES];
} Part;

typedef struct Patch {
	const char*       name; /* the patch's name in XWAU's patcher */
	XwauExePatchGroup group;
	size_t            field;
	Part              parts[MAX_PARTS];
} Patch;

/* From XWAU's "Classic EXE patches" list, except the two-launcher patch,
   which TFTC's executables have but the list doesn't, and the turret patch,
   which XWAU's installer applies. */
static const Patch g_patches[] = {
	{ "Beam system for rebel starfighters",
	  XWAU_EXE_PATCHES_CRAFT,
	  offsetof(XwauExePatches, craft_beams),
	  { { 0x41C6BF, 2, { 0x74, 0x1E }, { 0xEB, 0x29 } } } },
	{ "disable pilots ejecting",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, no_pilot_ejections),
	  { { 0x4042F4, 6, { 0x38, 0x1D, 0x18, 0x33, 0x77, 0x00 }, { 0xEB, 0xEB, 0x90, 0x90, 0x90, 0x90 } } } },
	{ "disable escape pods ejecting",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, no_escape_pod_ejections),
	  { { 0x40434B, 5, { 0xE8, 0xC0, 0x0A, 0x09, 0x00 }, { 0xEB, 0x94, 0x90, 0x90, 0x90 } } } },
	{ "Set team to 9 for escape pods / ejected pilots",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, ejected_craft_team_9),
	  { { 0x404481,
		  7,
		  { 0x8A, 0x0C, 0x45, 0xF1, 0xDC, 0x80, 0x00 },
		  { 0xB1, 0x09, 0x90, 0x90, 0x90, 0x90, 0x90 } },
		{ 0x404693, 6, { 0x8A, 0x81, 0x98, 0x00, 0x00, 0x00 }, { 0xB0, 0x09, 0x90, 0x90, 0x90, 0x90 } } } },
	{ "Set escape pods / pilots ejections order to null",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, ejected_craft_skip_setup),
	  { { 0x404EEB, 6, { 0x0F, 0x87, 0xF4, 0x00, 0x00, 0x00 }, { 0x90, 0xE9, 0xF4, 0x00, 0x00, 0x00 } } } },
	{ "enable escape pod / pilots ejection on skirmish missions",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, skirmish_ejections),
	  { { 0x5797B6,
		  8,
		  { 0xC6, 0x84, 0x29, 0xC9, 0x0D, 0x00, 0x00, 0x16 },
		  { 0xC6, 0x84, 0x29, 0xC9, 0x0D, 0x00, 0x00, 0x00 } } } },
	{ "Prevent AI warhead firing from blocking the 'attacked' trigger condition",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, ai_warheads_attack_on_hit),
	  { { 0x4A82E2,
		  8,
		  { 0xC6, 0x84, 0x19, 0x8C, 0x01, 0x00, 0x00, 0x01 },
		  { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 } } } },
	{ "Jamming beam always completely shuts down turrets",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, one_jamming_beam_stops_turrets),
	  { { 0x4E184A, 6, { 0x81, 0xF9, 0x00, 0x80, 0x02, 0x00 }, { 0x81, 0xF9, 0xFF, 0xFF, 0x00, 0x00 } } } },
	{ "Disable gun turret random rotation",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, no_turret_idle_spin),
	  { { 0x40215A, 2, { 0xA8, 0x01 }, { 0xEB, 0x27 } } } },
	{ "first launcher of craft with two launcher types loads advanced missiles",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, two_launcher_advanced_missiles),
	  { { 0x41BC0E, 2, { 0x1F, 0x01 }, { 0x24, 0x01 } } } },
};

/* Whether `patch` is applied. Reports bytes that are neither the stock nor
   the patched ones, and a patch with only some of its parts. */
static bool applied(const XwauGameExe* exe, const Patch* patch) {
	size_t parts = 0, patched = 0;
	for (; parts < MAX_PARTS && patch->parts[parts].size; ++parts) {
		const Part*    part  = &patch->parts[parts];
		const uint8_t* bytes = XwauGameExe_At(exe, part->address, part->size);
		if (bytes && memcmp(bytes, part->patched, part->size) == 0)
			++patched;
		else if (!bytes || memcmp(bytes, part->stock, part->size) != 0) {
			ReportUnsupported("unknown code at 0x%X of XWINGALLIANCE.EXE, where the patch \"%s\" goes",
							  (unsigned)part->address, patch->name);
			return false;
		}
	}
	if (patched != 0 && patched != parts) {
		ReportUnsupported("only part of the patch \"%s\" in XWINGALLIANCE.EXE", patch->name);
		return false;
	}
	return patched != 0;
}

void XwauExePatches_Read(const XwauGameExe* exe, XwauExePatchGroup group, XwauExePatches* out) {
	memset(out, 0, sizeof *out);
	if (!exe)
		return;
	for (size_t i = 0; i < sizeof g_patches / sizeof g_patches[0]; ++i)
		if (g_patches[i].group == group)
			*(bool*)((char*)out + g_patches[i].field) = applied(exe, &g_patches[i]);
}
