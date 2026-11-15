#include "xwau_exe_patches.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "util.h"
#include "xwau_game_tables.h"

enum { MAX_PARTS = 3, MAX_BYTES = 12 };

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
	const char*       rule; /* the rule that the flight settings get, or NULL */
	Part              parts[MAX_PARTS];
} Patch;

/* From XWAU's "Classic EXE patches" list, except the two-launcher patch,
   which TFTC's executables have but the list doesn't, and the turret patch,
   which XWAU's installer applies. The ones after it are only in TFTC-C's
   executable. */
static const Patch g_patches[] = {
	{ "Beam system for rebel starfighters",
	  XWAU_EXE_PATCHES_CRAFT,
	  offsetof(XwauExePatches, craft_beams),
	  NULL,
	  { { 0x41C6BF, 2, { 0x74, 0x1E }, { 0xEB, 0x29 } } } },
	{ "disable pilots ejecting",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, no_pilot_ejections),
	  "no_pilot_ejections",
	  { { 0x4042F4, 6, { 0x38, 0x1D, 0x18, 0x33, 0x77, 0x00 }, { 0xEB, 0xEB, 0x90, 0x90, 0x90, 0x90 } } } },
	{ "disable escape pods ejecting",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, no_escape_pod_ejections),
	  "no_escape_pod_ejections",
	  { { 0x40434B, 5, { 0xE8, 0xC0, 0x0A, 0x09, 0x00 }, { 0xEB, 0x94, 0x90, 0x90, 0x90 } } } },
	{ "Set team to 9 for escape pods / ejected pilots",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, ejected_craft_team_9),
	  "ejected_craft_team_9",
	  { { 0x404481,
		  7,
		  { 0x8A, 0x0C, 0x45, 0xF1, 0xDC, 0x80, 0x00 },
		  { 0xB1, 0x09, 0x90, 0x90, 0x90, 0x90, 0x90 } },
		{ 0x404693, 6, { 0x8A, 0x81, 0x98, 0x00, 0x00, 0x00 }, { 0xB0, 0x09, 0x90, 0x90, 0x90, 0x90 } } } },
	{ "Set escape pods / pilots ejections order to null",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, ejected_craft_skip_setup),
	  "ejected_craft_skip_setup",
	  { { 0x404EEB, 6, { 0x0F, 0x87, 0xF4, 0x00, 0x00, 0x00 }, { 0x90, 0xE9, 0xF4, 0x00, 0x00, 0x00 } } } },
	{ "enable escape pod / pilots ejection on skirmish missions",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, skirmish_ejections),
	  "skirmish_ejections",
	  { { 0x5797B6,
		  8,
		  { 0xC6, 0x84, 0x29, 0xC9, 0x0D, 0x00, 0x00, 0x16 },
		  { 0xC6, 0x84, 0x29, 0xC9, 0x0D, 0x00, 0x00, 0x00 } } } },
	{ "Prevent AI warhead firing from blocking the 'attacked' trigger condition",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, ai_warheads_attack_on_hit),
	  "ai_warheads_attack_on_hit",
	  { { 0x4A82E2,
		  8,
		  { 0xC6, 0x84, 0x19, 0x8C, 0x01, 0x00, 0x00, 0x01 },
		  { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 } } } },
	{ "Jamming beam always completely shuts down turrets",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, one_jamming_beam_stops_turrets),
	  "one_jamming_beam_stops_turrets",
	  { { 0x4E184A, 6, { 0x81, 0xF9, 0x00, 0x80, 0x02, 0x00 }, { 0x81, 0xF9, 0xFF, 0xFF, 0x00, 0x00 } } } },
	{ "Disable gun turret random rotation",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, no_turret_idle_spin),
	  "no_turret_idle_spin",
	  { { 0x40215A, 2, { 0xA8, 0x01 }, { 0xEB, 0x27 } } } },
	{ "first launcher of craft with two launcher types loads advanced missiles",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, two_launcher_advanced_missiles),
	  "two_launcher_advanced_missiles",
	  { { 0x41BC0E, 2, { 0x1F, 0x01 }, { 0x24, 0x01 } } } },
	/* From the Classic list. */
	{ "Disable dual warheads for top AI",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, no_top_ai_dual_warheads),
	  "no_top_ai_dual_warheads",
	  { { 0x4A820A, 2, { 0x75, 0x18 }, { 0xEB, 0x18 } } } },
	{ "Disable starship warhead defence",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, no_turret_warhead_self_defense),
	  "no_turret_warhead_self_defense",
	  { { 0x4A9069, 6, { 0x0F, 0x85, 0xE1, 0x00, 0x00, 0x00 }, { 0x90, 0xE9, 0xE1, 0x00, 0x00, 0x00 } } } },
	{ "Disable starship warhead defence if Protect order is given",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, no_turret_warhead_escort_defense),
	  "no_turret_warhead_escort_defense",
	  { { 0x4A624B, 6, { 0x0F, 0x84, 0xAE, 0x01, 0x00, 0x00 }, { 0xE9, 0xAF, 0x01, 0x00, 0x00, 0x90 } } } },
	{ "Disable Explosion Splash Damage",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, no_explosion_splash_damage),
	  "no_explosion_splash_damage",
	  { { 0x412280, 3, { 0x83, 0xEC, 0x40 }, { 0xC3, 0x90, 0x90 } } } },
	/* A hook_weapon_lock with the weapon lock behaviors hook sets this byte
	   itself on each hit, so the flight settings only use the patch without
	   that hook (xwau_weapon_lock.h). */
	{ "Mag Pulse Laser Behaviour",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, mag_pulse_drain_only),
	  NULL,
	  { { 0x40EA21, 2, { 0x74, 0x65 }, { 0xEB, 0x65 } } } },
	{ "Skip checking for engine wash",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, no_engine_wash),
	  "no_engine_wash",
	  { { 0x408E91, 6, { 0x0F, 0x8E, 0x83, 0x00, 0x00, 0x00 }, { 0xE9, 0x84, 0x00, 0x00, 0x00, 0x90 } } } },
	{ "AI shield recharge maximum from 200% to 100%",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, ai_shields_charge_to_full),
	  "ai_shields_charge_to_full",
	  { { 0x490499, 3, { 0xC1, 0xE0, 0x02 }, { 0xC1, 0xE0, 0x01 } } } },
	{ "Reduce attacker limit against player according to difficulty",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, player_attacker_limit_by_difficulty),
	  "player_attacker_limit_by_difficulty",
	  { { 0x4A6A9C,
		  12,
		  { 0x66, 0x48, 0x66, 0xF7, 0xD8, 0x1B, 0xC0, 0x24, 0xA0, 0x83, 0xC0, 0x64 },
		  { 0xB8, 0x04, 0x00, 0x00, 0x00, 0xEB, 0x05, 0x90, 0x90, 0x90, 0x90, 0x90 } },
		{ 0x4A6AB6,
		  12,
		  { 0x66, 0x48, 0x66, 0xF7, 0xD8, 0x1B, 0xC0, 0x24, 0xFB, 0x83, 0xC0, 0x08 },
		  { 0xB8, 0x03, 0x00, 0x00, 0x00, 0xEB, 0x05, 0x90, 0x90, 0x90, 0x90, 0x90 } },
		{ 0x4A6AD0,
		  12,
		  { 0x66, 0x48, 0x66, 0xF7, 0xD8, 0x1B, 0xC0, 0x24, 0xFE, 0x83, 0xC0, 0x04 },
		  { 0xB8, 0x02, 0x00, 0x00, 0x00, 0xEB, 0x05, 0x90, 0x90, 0x90, 0x90, 0x90 } } } },
	{ "Remove 1/3 throttle turn rate mechanic",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, no_throttle_turn_rate),
	  "no_throttle_turn_rate",
	  { { 0x4FA660,
		  7,
		  { 0x66, 0x3D, 0x55, 0x55, 0x73, 0x09, 0x8D },
		  { 0xB9, 0xFF, 0xFF, 0x00, 0x00, 0xEB, 0x1E } } } },
	{ "Remove LES turn rate mechanic",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, no_energy_turn_rate),
	  "no_energy_turn_rate",
	  { { 0x4FA70E, 5, { 0xBF, 0x04, 0x00, 0x00, 0x00 }, { 0xBF, 0x00, 0x00, 0x00, 0x00 } },
		{ 0x4FA713, 2, { 0x2B, 0xF8 }, { 0x90, 0x90 } } } },
	{ "Remove AI laser convergence",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, no_ai_laser_convergence),
	  "no_ai_laser_convergence",
	  { { 0x4A784B,
		  7,
		  { 0x8A, 0x84, 0x40, 0x8D, 0xB4, 0x5B, 0x00 },
		  { 0xB0, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90 } } } },
	{ "Remove player laser convergence",
	  XWAU_EXE_PATCHES_FLIGHT,
	  offsetof(XwauExePatches, no_player_laser_convergence),
	  "no_player_laser_convergence",
	  { { 0x4FC0D3,
		  7,
		  { 0x8A, 0x84, 0x40, 0x8D, 0xB4, 0x5B, 0x00 },
		  { 0xB0, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90 } },
		{ 0x41BA49, 2, { 0x74, 0x0E }, { 0xEB, 0x0E } } } },
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

static bool patch_flag(const XwauExePatches* patches, const Patch* patch) {
	return *(const bool*)((const char*)patches + patch->field);
}

bool XwauExePatches_AnyRule(const XwauExePatches* patches) {
	for (size_t i = 0; i < sizeof g_patches / sizeof g_patches[0]; ++i)
		if (g_patches[i].rule && patch_flag(patches, &g_patches[i]))
			return true;
	return false;
}

void XwauExePatches_WriteRules(FILE* f, const XwauExePatches* patches) {
	for (size_t i = 0; i < sizeof g_patches / sizeof g_patches[0]; ++i)
		if (g_patches[i].rule && patch_flag(patches, &g_patches[i]))
			fprintf(f, "  %s: true\n", g_patches[i].rule);
}

void XwauExePatches_Read(const XwauGameExe* exe, XwauExePatchGroup group, XwauExePatches* out) {
	memset(out, 0, sizeof *out);
	if (!exe)
		return;
	for (size_t i = 0; i < sizeof g_patches / sizeof g_patches[0]; ++i)
		if (g_patches[i].group == group)
			*(bool*)((char*)out + g_patches[i].field) = applied(exe, &g_patches[i]);
}
