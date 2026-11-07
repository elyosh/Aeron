#ifndef AERON_XWAU_WEAPON_LOCK_H
#define AERON_XWAU_WEAPON_LOCK_H

/*
 * Reads the sections of hook_weapon_lock other than a model's
 * [WarheadLock]:
 *   - [AttackBreakDistance] sets how close an AI craft gets to its target
 *     before it breaks off, from how far it fires its lasers, and whether it
 *     dodges warheads and attackers. It becomes the `ai_attack` group.
 *   - [WeaponLockBehaviors] sets what a mag pulse hit does to the craft. It
 *     becomes the `mag_pulse` group.
 *   - The global [WarheadLock] section sets the AI's warhead lock ranges
 *     against big targets, by the genus of the AI's own craft. It becomes
 *     `warhead_lock.ai_lock_range_m` in flight/flight.yaml.
 *
 * A model's section comes from FlightModels/<model><Section>.txt, or else
 * from the [<Section>] section of the model's INI. It goes into the model's
 * file. The global section comes from FlightModels/<Section>.txt, or else
 * from the [<Section>] section of FlightModels/default.ini. It goes into
 * flight/flight.yaml, with the same shape. A key of the model replaces the
 * same key of the global section.
 *
 * Each section is only read when the mod's hook_weapon_lock.dll has the
 * feature, since the mods don't ship the same hook versions. The DLL's
 * strings show which features it has.
 *
 * Distances are written in metres, and times in milliseconds. The hook's
 * "second" is 236 ticks of 4 ms.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "xwau_names.h"

/* The features of the mod's hook_weapon_lock.dll. */
typedef struct XwauWeaponLockFeatures {
	bool attack_break; /* [AttackBreakDistance] */
	bool avoid_hits;   /* its AIAvoidHitOrder key */
	bool mag_pulse;    /* [WeaponLockBehaviors] */
	bool global_lock;  /* the global [WarheadLock] keys */
} XwauWeaponLockFeatures;

/* Finds the features of the hook_weapon_lock.dll of the installation
 * `game`. Returns false, with every feature off, when it has no such
 * DLL. */
bool XwauWeaponLock_Features(const char* game, XwauWeaponLockFeatures* out);

enum { XWAU_AI_ATTACK_RANKS = 6 };

/* The values of [AttackBreakDistance], in game units. */
typedef enum XwauAiAttackValue {
	XWAU_AI_ATTACK_WARHEAD,
	XWAU_AI_ATTACK_SMALL, /* 6 values, by rank */
	XWAU_AI_ATTACK_BIG_MAX = XWAU_AI_ATTACK_SMALL + XWAU_AI_ATTACK_RANKS,
	XWAU_AI_ATTACK_LASER,                                                    /* 6 values, by rank */
	XWAU_AI_ATTACK_AVOID_HITS = XWAU_AI_ATTACK_LASER + XWAU_AI_ATTACK_RANKS, /* 0 or 1 */
	XWAU_AI_ATTACK_VALUES
} XwauAiAttackValue;

/* The break offset that a Model_<name> key adds for targets of model
 * `model`. */
typedef struct XwauAiAttackOffset {
	char model[128];
	int  offset;
} XwauAiAttackOffset;

/* An [AttackBreakDistance] section. */
typedef struct XwauAiAttack {
	bool                has;
	bool                set[XWAU_AI_ATTACK_VALUES];
	int                 value[XWAU_AI_ATTACK_VALUES];
	XwauAiAttackOffset* offsets;
	size_t              offset_count;
} XwauAiAttack;

/* Reads an [AttackBreakDistance] section: `txt` when it has lines, else the
 * section of `ini`. Either path can be NULL. `game` is the installation, to
 * check the models of the Model_<name> keys. `features` tells which keys the
 * DLL reads. A value that isn't a number would crash XWAU, so it is reported
 * and left out. A value that the game can't use makes the import fail.
 * Returns false when out of memory. */
bool XwauAiAttack_Read(const char* game, const char* txt, const char* ini,
					   const XwauWeaponLockFeatures* features, XwauAiAttack* out);
/* Reads the global [AttackBreakDistance] section of `game`. */
bool XwauAiAttack_ReadGlobal(const char* game, const XwauWeaponLockFeatures* features, XwauAiAttack* out);
void XwauAiAttack_Free(XwauAiAttack* attack);
/* Writes the `ai_attack` group of `attack`, with the rank names in
 * `names`. */
void XwauAiAttack_Write(FILE* f, const XwauNames* names, const XwauAiAttack* attack);

/* A [WeaponLockBehaviors] section. */
typedef struct XwauMagPulse {
	bool has;
	int  drain_only;       /* -1: not set, else 0 or 1 */
	int  disable_ticks[2]; /* -1: not set. A model only has [0]. The global
							  section has small craft in [0] and large craft
							  in [1]. */
} XwauMagPulse;

/* Reads a model's [WeaponLockBehaviors] section: `txt` when it has lines,
 * else the section of `ini`. Either path can be NULL. -1 is "not set". A
 * value that isn't a number is reported and left out. A time that the game
 * can't use makes the import fail. */
void XwauMagPulse_Read(const char* txt, const char* ini, XwauMagPulse* out);
/* Reads the global [WeaponLockBehaviors] section of `game`. */
void XwauMagPulse_ReadGlobal(const char* game, XwauMagPulse* out);
/* Writes the `mag_pulse` group of `pulse`. `global` writes the times of the
 * global section, by size. */
void XwauMagPulse_Write(FILE* f, const XwauMagPulse* pulse, bool global);

/* Writes the `ai_attack`, `mag_pulse` and `warhead_lock` sections of
 * flight/flight.yaml for the installation `game`. `ai_attack` gets the AI
 * laser ranges by rank whenever the DLL reads [AttackBreakDistance], since
 * the hook then uses them even with no data. Sets `ai_attack`, `mag_pulse`
 * and `warhead_lock` to whether each section was written. Returns false
 * when out of memory. */
bool XwauWeaponLock_WriteFlightSettings(FILE* f, const char* game, const XwauNames* names, bool* ai_attack,
										bool* mag_pulse, bool* warhead_lock);

#endif /* AERON_XWAU_WEAPON_LOCK_H */
