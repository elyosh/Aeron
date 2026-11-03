#ifndef AERON_XWAU_GUNNER_SEATS_H
#define AERON_XWAU_GUNNER_SEATS_H

/*
 * Imports the gunner seats of XWAU's hook_mission_objects, in the mod
 * importer's craft phase. A model's seats come from
 * FlightModels/<model>Turrets.txt, or else from the [Turrets] section of its
 * INI. Each line with at least 9 values, separated by ',' or ';', is one
 * seat:
 *
 *   PositionX, PositionY, PositionZ, OrientationX, OrientationY, ArcX, ArcY, OptModel, IsInverted
 *   0, 19, 250, -16384, 16384, 12040, 17308, MillenniumFalcon2Gunner, 0
 *
 * The values go into the `gunner_seats` group of the model file:
 *
 *   gunner_seats:
 *     forward_fire: false
 *     seats:
 *       - {position_m: [0.464, 0, 6.104], mount_pitch_degrees: 90, mount_yaw_degrees: -90,
 *          aim_limit_pitch_degrees: 66.138, aim_limit_yaw_degrees: 95.076,
 *          cockpit: millenniumfalcon2gunner, inverted: false}
 *
 * The hook's value names don't match their meaning. They follow the order of
 * the craft record fields: PositionX is the record's y, PositionY its x,
 * OrientationX its yaw (mount angle b) and OrientationY its pitch (mount
 * angle a). ArcX and ArcY are the pitch and yaw aim limits. The importer
 * writes the position in metres and the angles in degrees, with three
 * decimals. Like the hook, it reads the numbers with std::stoi(value, 0, 0)
 * and cuts them to 16 bits. A line with numbers that can't be read would
 * crash XWAU, so it is left out. Lines with fewer than 9 values aren't
 * seats. The hook only reads the TurretForwardFire key from them. A section
 * can set `TurretForwardFire = 0`, which gives `forward_fire: false`. A
 * TurretForwardFire value that isn't a number would crash XWAU, so it is
 * left out.
 *
 * A model with the section gets the group even without seat lines. The hook
 * then gives it no seats. A model without the section gets the seats of its
 * craft record, if it has some, the same way the hook builds them: the first
 * seat's cockpit is <model>Gunner, the second seat's is <model>Gunner2, else
 * <model>Gunner, and the second seat is upside down. If a cockpit's OPT isn't
 * in FlightModels, the seat uses the stock CorellianTransportGunner, like
 * the hook. The game has room for 2 seats. More seats make the import
 * fail.
 *
 * XWAU loads all seat cockpits into object type 317. The game gives each
 * cockpit model its own object type instead.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "xwau_config.h"
#include "xwau_game_tables.h"

enum { XWAU_GUNNER_SEAT_MAX = 2 };

typedef struct XwauGunnerSeat {
	int  x, y, z, mount_angle_a, mount_angle_b, aim_limit_a, aim_limit_b;
	bool inverted;
	char cockpit[256]; /* the cockpit's model name */
} XwauGunnerSeat;

typedef struct XwauGunnerSeats {
	bool           has; /* the model has the section, or seats in its record */
	bool           forward_fire;
	size_t         count;
	XwauGunnerSeat seats[XWAU_GUNNER_SEAT_MAX];
} XwauGunnerSeats;

/* Reads the seats of model `model` (its name in FlightModels) of the
 * installation `game`: `turrets_txt` (<model>Turrets.txt) when it has lines,
 * else the [Turrets] section of `ini`, else the record of the first object
 * type of `tables` that loads the model. Any of the three can be NULL. */
void XwauGunnerSeats_Read(const char* game, const XwauGameTables* tables, const char* model,
						  const char* turrets_txt, const char* ini, XwauGunnerSeats* out);

/* Writes the `gunner_seats` group of `seats`, when it has one, and adds the
 * names of its cockpits to `cockpits`, which can be NULL. Returns false when
 * out of memory. */
bool XwauGunnerSeats_Write(FILE* f, const XwauGunnerSeats* seats, XwauLines* cockpits);

/* Writes aim limit `steps` as degrees. The game reads the limit as a signed
 * 16-bit angle, so a limit of 32768 or more locks the aim instead of
 * limiting it. Such a limit makes the import fail, with `label` in the
 * report, and is written as 180. */
void XwauGunnerSeats_WriteAimLimit(FILE* f, const char* label, int steps);

/* Checks each cockpit in `cockpits`, except the stock one. Reports a cockpit
 * whose GLB isn't in <output_dir>/models. `game` isn't used. */
void XwauGunnerSeats_CheckCockpits(const char* game, const char* output_dir, const XwauLines* cockpits);

#endif /* AERON_XWAU_GUNNER_SEATS_H */
