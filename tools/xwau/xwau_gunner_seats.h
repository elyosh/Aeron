#ifndef AERON_XWAU_GUNNER_SEATS_H
#define AERON_XWAU_GUNNER_SEATS_H

/*
 * Imports the gunner seats of XWAU's hook_mission_objects (xwau_import
 * craft). A model's seats come from FlightModels/<model>Turrets.txt, or else
 * from the [Turrets] section of its INI. Each line with at least 9 values,
 * split at ',' and ';', is one seat:
 *
 *   PositionX, PositionY, PositionZ, OrientationX, OrientationY, ArcX, ArcY, OptModel, IsInverted
 *   0, 19, 250, -16384, 16384, 12040, 17308, MillenniumFalcon2Gunner, 0
 *
 * The values go into the `gunner_seats` group of the model file:
 *
 *   gunner_seats:
 *     forward_fire: false
 *     seats:
 *       - {x: 19, y: 0, z: 250, mount_angle_a: 16384, mount_angle_b: -16384, aim_limit_a: 12040,
 *          aim_limit_b: 17308, cockpit: millenniumfalcon2gunner, inverted: false}
 *
 * The hook's names follow the order of the craft record's fields, so
 * PositionX is the record's y, PositionY its x, OrientationX its
 * mount_angle_b and OrientationY its mount_angle_a. ArcX and ArcY are
 * aim_limit_a and aim_limit_b. The numbers are read with std::stoi(value, 0,
 * 0) and cut to 16 bits, as the hook does. A line whose numbers can't be
 * read would crash XWAU; it is reported and left out. A section can also set
 * `TurretForwardFire = 0`, which gives `forward_fire: false`.
 *
 * A model with the section gets the group, also when it has no seat line:
 * the hook then gives it no seat. A model without the section gets the seats
 * of its craft record, if it has some, as the hook builds them: the first
 * seat's cockpit is <model>Gunner, the second one's <model>Gunner2, else
 * <model>Gunner, and the second seat is upside down. A cockpit whose OPT
 * isn't in FlightModels is the stock CorellianTransportGunner, as in the
 * hook; it is reported. The game has room for 2 seats; more are reported and
 * left out.
 *
 * Every cockpit other than the stock one gets an object type file of its
 * own, in a reserved slot with its own empty craft record, with
 * `gunner_cockpit: true` (craft_definitions.h in the game). XWAU loads every
 * seat's cockpit into object type 317; OpenXWA keeps them apart.
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

/* Writes an object type file for each cockpit in `cockpits` but the stock
 * one into <output_dir>/object_types. `game` is the installation and
 * `stock_game` the stock game, to find the free reserved slots. Run it after
 * XwauObjects_Write, which removes the files of the last run. Returns false
 * on an error. */
bool XwauGunnerSeats_WriteCockpits(const char* game, const char* stock_game, const char* output_dir,
								   const XwauLines* cockpits);

#endif /* AERON_XWAU_GUNNER_SEATS_H */
