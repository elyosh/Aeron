#ifndef AERON_XWAU_EXE_PATCHES_H
#define AERON_XWAU_EXE_PATCHES_H

/*
 * The code patches of a mod's XWINGALLIANCE.EXE that the importer converts.
 * Each patch is found by its bytes, as listed by XWAU's patcher. A patch is
 * applied when every part of it has the patched bytes. Other bytes are
 * reported as unsupported, and the patch counts as not applied.
 *
 * The patches come in two groups, one for each phase of the import that
 * uses them, so that each phase reports only its own patches.
 *
 * The craft phase (xwau_objects.h):
 *   - 0x41C6BF: the X-wing, Y-wing, A-wing, B-wing, Z-95 and TIE Fighter
 *     can carry a beam (mission.c:6624-6627).
 *
 * The flight settings phase, which writes rules (xwau_mission_settings.h).
 * TFTC's executables have all nine patches, and XWAU 2025's only the ones
 * at 0x404481, 0x404EEB, 0x5797B6 and 0x40215A:
 *   - 0x4042F4: no pilot ejects from a fighter;
 *   - 0x40434B: no escape pod ejects from a transport, starship or
 *     platform;
 *   - 0x404481 and 0x404693: pods and pilots are on team 9;
 *   - 0x404EEB: pods don't get their AI plan, and pilots keep the random
 *     speed and direction that every ejected craft starts with;
 *   - 0x5797B6: the craft of a skirmish mission can eject;
 *   - 0x4A82E2: an AI warhead marks its target as attacked when it hits,
 *     not when it's launched;
 *   - 0x4E184A: one jamming beam stops a craft's turrets, instead of three;
 *   - 0x41BC0E: the first launcher of a craft with two launcher types loads
 *     advanced missiles instead of missiles;
 *   - 0x40215A: a rotary gun turret without a target doesn't turn, and
 *     doesn't call GameRand.
 *
 * TFTC-C's executable also has 12 patches of the Classic list that the
 * others don't. Each gives the rule of the same name, except the Mag Pulse
 * patch, which gives `mag_pulse.drain_only`:
 *   - 0x4A820A: no_top_ai_dual_warheads;
 *   - 0x4A9069: no_turret_warhead_self_defense;
 *   - 0x4A624B: no_turret_warhead_escort_defense;
 *   - 0x412280: no_explosion_splash_damage;
 *   - 0x40EA21: a Mag Pulse hit on a player only drains its lasers;
 *   - 0x408E91: no_engine_wash;
 *   - 0x490499: ai_shields_charge_to_full;
 *   - 0x4A6A9C, 0x4A6AB6 and 0x4A6AD0: player_attacker_limit_by_difficulty;
 *   - 0x4FA660: no_throttle_turn_rate;
 *   - 0x4FA70E and 0x4FA713: no_energy_turn_rate;
 *   - 0x4A784B: no_ai_laser_convergence;
 *   - 0x4FC0D3 and 0x41BA49: no_player_laser_convergence.
 *
 * TFTC's executables also fix XwaAIComponentGoneOrder, which reads through
 * a null pointer in the original. The port always has that fix, so the
 * importer doesn't look for it.
 */

#include <stdbool.h>
#include <stdio.h>

#include "xwau_game_tables.h"

typedef enum XwauExePatchGroup {
	XWAU_EXE_PATCHES_CRAFT,
	XWAU_EXE_PATCHES_FLIGHT,
} XwauExePatchGroup;

typedef struct XwauExePatches {
	/* The patch of the craft group. */
	bool craft_beams;
	/* The patches of the flight group. */
	bool no_pilot_ejections;
	bool no_escape_pod_ejections;
	bool ejected_craft_team_9;
	bool ejected_craft_skip_setup;
	bool skirmish_ejections;
	bool ai_warheads_attack_on_hit;
	bool one_jamming_beam_stops_turrets;
	bool two_launcher_advanced_missiles;
	bool no_turret_idle_spin;
	bool no_top_ai_dual_warheads;
	bool no_turret_warhead_self_defense;
	bool no_turret_warhead_escort_defense;
	bool no_explosion_splash_damage;
	bool mag_pulse_drain_only;
	bool no_engine_wash;
	bool ai_shields_charge_to_full;
	bool player_attacker_limit_by_difficulty;
	bool no_throttle_turn_rate;
	bool no_energy_turn_rate;
	bool no_ai_laser_convergence;
	bool no_player_laser_convergence;
} XwauExePatches;

/* Reads the patches of `group` in `exe`. The patches of the other group stay
 * false. Without an executable (NULL), there are none. */
void XwauExePatches_Read(const XwauGameExe* exe, XwauExePatchGroup group, XwauExePatches* out);
/* Returns whether `patches` has a patch that gives a rule. */
bool XwauExePatches_AnyRule(const XwauExePatches* patches);
/* Writes the rules of the patches in `patches`, one `  <rule>: true` line
 * each. */
void XwauExePatches_WriteRules(FILE* f, const XwauExePatches* patches);

#endif /* AERON_XWAU_EXE_PATCHES_H */
