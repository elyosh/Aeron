#include "model_tables.h"

#include <string.h>
#include <strings.h>

typedef struct Eye {
	const char* model;
	int         eye[3];
} Eye;

/* The craft table of XWAU 2025 Full 1.0.0, from the XWAU2025_Full_1.0.0.202
 * patch of its installer. The 1.1.0 update changes no eye. Only the entries
 * with a name are listed, because hook_mission_objects matches that name to
 * the OPT name. */
static const Eye kXwauEyes[] = {
	{ "Xwing", { 0, 33, 32 } },
	{ "Ywing", { 0, 187, 13 } },
	{ "Awing", { 0, 2, 31 } },
	{ "Bwing", { 0, 50, 204 } },
	{ "TieFighter", { 0, -3, 3 } },
	{ "TieInterceptor", { 0, -3, 3 } },
	{ "TieBomber", { 52, 37, 3 } },
	{ "TieAvenger", { 0, -3, 3 } },
	{ "TieDefender", { 0, -3, 3 } },
	{ "IrdFighter", { 0, 105, 8 } },
	{ "ToscanFighter", { 0, 94, 25 } },
	{ "MissileBoat", { 0, 126, 22 } },
	{ "Twing", { 0, 0, 31 } },
	{ "Z-95", { 0, -19, 33 } },
	{ "R41Starchaser", { 0, 26, 11 } },
	{ "AssaultGunboat", { 0, 39, 18 } },
	{ "SkiprayBlastboat", { 0, 347, 81 } },
	{ "TieBizarro", { -124, 2, 5 } },
	{ "TieBigGun", { 0, 2, 5 } },
	{ "TieWarheads", { 0, 2, 5 } },
	{ "TieBooster", { 0, 2, 5 } },
	{ "CloakShapeFighter", { 0, 275, 72 } },
	{ "RazorFighter", { 0, 137, 42 } },
	{ "PlanetaryFighter", { 0, 216, 58 } },
	{ "SupaFighter", { -149, 238, 35 } },
	{ "PinookFighter", { -26, 116, 44 } },
	{ "TieAdvancedX1", { 0, -3, 3 } },
	{ "PreybirdFighter", { 0, 157, 26 } },
	{ "SlaveOne", { 0, 135, 50 } },
	{ "Pursuer1", { 0, -382, 168 } },
	{ "UWing", { 13, -82, 37 } },
	{ "Shuttle", { 35, 160, 96 } },
	{ "EscortShuttle", { 0, -30, 15 } },
	{ "CorellianTransport2", { 463, 325, 110 } },
	{ "MillenniumFalcon2", { 463, 325, 110 } },
	{ "Outrider", { 462, 293, 15 } },
	{ "LuxuryYacht", { -21, 456, 46 } },
	{ "FamilyTransport", { 0, 628, 25 } },
	{ "Ghost", { 0, 605, -71 } },
};

/* The craft table of the original game. Only the entries with a name are
 * listed. These are the player's craft, and the game builds their cockpit file
 * names from that name. Each entry is listed under the model that its craft
 * type uses. An unnamed entry (OBJ_XWing_030) also uses the X-wing model, with
 * the eye (0, -30, 15). */
static const Eye kStockEyes[] = {
	{ "assaultgunboat", { 0, -30, 15 } },
	{ "awing", { 0, -30, 22 } },
	{ "bwing", { -1, -17, 9 } },
	{ "cloakshapefighter", { 0, -30, 15 } },
	{ "corelliantransport2", { 174, -121, 33 } },
	{ "familytransport", { 6, 636, 25 } },
	{ "milleniumfalcon2", { 174, -125, 33 } },
	{ "missileboat", { 0, -30, 15 } },
	{ "r41", { 0, -30, 15 } },
	{ "skiprayblastboat", { 0, -30, 15 } },
	{ "tieadvanced", { 0, -8, 6 } },
	{ "tiebizarro", { 0, -8, 6 } },
	{ "tiebomber", { 76, -8, 6 } },
	{ "tiebooster", { 0, 24, 4 } },
	{ "tiedefender", { 0, -8, 6 } },
	{ "tiefighter", { 0, -8, 6 } },
	{ "tieinterceptor", { 0, -8, 6 } },
	{ "xwing", { 1, -22, 34 } },
	{ "ywing", { -2, 299, 35 } },
	{ "z-95", { 0, 65, 29 } },
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static bool find_eye(const Eye* eyes, size_t count, const char* model, int eye[3]) {
	for (size_t i = 0; i < count; ++i) {
		if (strcasecmp(eyes[i].model, model) == 0) {
			memcpy(eye, eyes[i].eye, sizeof eyes[i].eye);
			return true;
		}
	}
	return false;
}

bool ModelTables_XwauEye(const char* model, int eye[3]) {
	return find_eye(kXwauEyes, COUNT(kXwauEyes), model, eye);
}

bool ModelTables_StockEye(const char* model, int eye[3]) {
	return find_eye(kStockEyes, COUNT(kStockEyes), model, eye);
}


typedef struct Entry {
	const char* model;
	const char* values;
} Entry;

static const char* find_entry(const Entry* entries, size_t count, const char* model) {
	for (size_t i = 0; i < count; ++i)
		if (strcasecmp(entries[i].model, model) == 0)
			return entries[i].values;
	return NULL;
}

/* Replacement models do not have the stock component layout. The recovered
 * code ties some behaviour to fixed component numbers, so these tags and
 * articulations drive that behaviour instead. They use the stock numbers. */
static const Entry kStockTags[] = {
	/* Crew animation (FlightObject_AnimateCrewMeshRotations). These are the
	 * stock ordinals of each craft type, on the craft model. The side motion
	 * is `random` and the forward motion is `random_pause`, both at angle 32
	 * and speed 2. The ordinals index the per-mesh rotations of the craft,
	 * which the game also applies to its exterior model. The crew meshes are
	 * exterior meshes, except on the Millennium Falcon, which has no exterior
	 * OPT. Component 9 of the Family Transport is a gun turret, and the game
	 * nods it like a crew head. */
	{ "XWING", "6=crew:32:2:random 9=crew:32:2:random_pause" },
	{ "AWING", "4=crew:32:2:random_pause" },
	{ "YWING", "8=crew:32:2:random 13=crew:32:2:random_pause" },
	{ "CORELLIANTRANSPORT2", "19=crew:32:2:random 17=crew:32:2:random_pause" },
	{ "FAMILYTRANSPORT", "13=crew:32:2:random 9=crew:32:2:random_pause" },
	{ "MILLENIUMFALCON2", "21=crew:32:2:random 19=crew:32:2:random_pause" },
	/* Platforms: components hidden when the flight group has a beam weapon
	 * (g_platformBeamDisabledComponentIds). Any beam hides hidden_by_beam, and
	 * every beam except the tractor beam also hides hidden_by_jamming_decoy.
	 * Only the ids that are meshes of the OPT are listed. Platform3 and
	 * Platform5 have none. */
	{ "PLATFORM1",
	  "5=platform.hidden_by_beam 6=platform.hidden_by_jamming_decoy 15=platform.hidden_by_jamming_decoy" },
	{ "PLATFORM2",
	  "3=platform.hidden_by_beam 5=platform.hidden_by_beam 9=platform.hidden_by_beam "
	  "10=platform.hidden_by_beam 1=platform.hidden_by_jamming_decoy 4=platform.hidden_by_jamming_decoy "
	  "7=platform.hidden_by_jamming_decoy 8=platform.hidden_by_jamming_decoy" },
	{ "PLATFORM4", "5=platform.hidden_by_beam 13=platform.hidden_by_beam 15=platform.hidden_by_beam "
				   "6=platform.hidden_by_jamming_decoy 11=platform.hidden_by_jamming_decoy "
				   "14=platform.hidden_by_jamming_decoy" },
	/* Hangar props animated by the hangar and family-base scenes. */
	{ "HANGARCRANE", "0=hangarcrane.arm" },
	{ "HANGARDROID", "3=hangardroid.nod" },
	{ "HANGARDROID2", "1=hangardroid2.swing 2=hangardroid2.spin" },
	{ "WORKDROID1", "0=workdroid1.spin 3=workdroid1.turn 4=workdroid1.nod.1 6=workdroid1.nod.2" },
	/* Pilots: the head and the four limbs all use the forward motion. */
	{ "REBELPILOT", "1=crew:32:2:random_pause 2=crew:32:2:random_pause 3=crew:32:2:random_pause "
					"4=crew:32:2:random_pause 5=crew:32:2:random_pause" },
	{ "IMPERIALPILOT", "1=crew:32:2:random_pause 2=crew:32:2:random_pause 3=crew:32:2:random_pause "
					   "4=crew:32:2:random_pause 5=crew:32:2:random_pause" },
	/* Proving grounds. */
	{ "ACCELRING2", "4=accelring.core" },
	{ "ACCELRING3", "4=accelring.core" },
	{ "COMPACTOR", "1=compactor.door.1 2=compactor.door.2 3=compactor.door.3 4=compactor.door.4" },
	{ "CENTRIFUGE", "3=centrifuge.spin.1 4=centrifuge.spin.2 12=centrifuge.spin.3 8=centrifuge.door.1 "
					"7=centrifuge.door.2 6=centrifuge.door.3" },
	{ "SMELTINGROOM",
	  "3=smeltingroom.turret.1 4=smeltingroom.turret.2 5=smeltingroom.turret.3 "
	  "6=smeltingroom.turret.4 7=smeltingroom.turret.5 8=smeltingroom.turret.6 2=smeltingroom.power.1 "
	  "10=smeltingroom.power.2 9=smeltingroom.target" },
	/* Death Star reactor. */
	{ "DSREACTORCORE", "1=dsreactorcore.regenerator 2=dsreactorcore.reactor" },
};

const char* ModelTables_StockTags(const char* model) {
	return find_entry(kStockTags, COUNT(kStockTags), model);
}

/* Projectile models: the rows of the craft list (Equipment0.LST) for the
 * projectile kinds of the player and of NPCs. */
static const char* const kProjectiles[] = {
	"LASERREBEL",        "LASERREBELTURBO", "LASERIMP",    "LASERIMPTURBO", "LASERION", "LASERIONTURBO",
	"CONCUSSIONMISSILE", "PROTONTORPEDO",   "HEAVYROCKET", "SPACEBOMB",     "MAGPULSE", "FLARE",
	"LASERIMPDS",        "DSLASERINTERNAL",
};

bool ModelTables_IsProjectile(const char* model) {
	for (size_t i = 0; i < COUNT(kProjectiles); ++i)
		if (strcasecmp(kProjectiles[i], model) == 0)
			return true;
	return false;
}

/* XWAU's materials make some crew textures Shadeless. XWAU's renderer then
 * draws them unlit at full texture brightness, which keeps faces readable in
 * its dark cockpits. As emission under physical lighting, they would look like
 * they glow. Cockpit glass is Shadeless too. XWAU composites it unlit after
 * tone mapping, but as emission its grime would glow. A transparent texture in
 * this list becomes a plain lit surface with its own alpha. Lights and
 * displays keep their material. */
static const Entry kLitTextures[] = {
	/* Pilot and crew faces. */
	{ "AWing", "AWFace XWFace" },
	{ "AWingExterior", "AWFace XWFace" },
	{ "XWing", "XWFace" },
	{ "XWingExterior", "XWFace" },
	{ "YWing", "XWFace YWFace" },
	{ "YWingExterior", "XWFace YWFace" },
	{ "Z-95", "Face Eyes Head" },
	{ "Z-95Exterior", "Face Eyes Head" },
	{ "FamilyTransport", "Eyes Hair Head" },
	{ "FamilyTransportExterior", "Eyes Hair Head" },
	{ "PreybirdFighter", "Pilot PilotHead" },
	{ "PreybirdFighterExterior", "Pilot PilotHead" },
	/* Crew figures. */
	{ "CorellianTransport2Cockpit", "Emon" },
	{ "MillenniumFalcon2Cockpit", "Chewbacca NienArms NienBoots NienChest NienEyes NienHands NienHead" },
	{ "MiniFalconCockpit", "Chewbacca NienArms NienBoots NienChest NienEyes NienHands NienHead" },
	{ "LuxuryYacht", "LandoHands LandoLegs LandoTorso" },
	{ "LuxuryYachtExterior", "LandoHands LandoLegs LandoTorso" },
	{ "AssaultShuttle", "TrooperFront TrooperTop WorkerFront WorkerTop" },
	/* Cockpit glass (windows, canopies) and visors. */
	{ "AwingCockpit", "TEX00029" },
	{ "PlanetaryFighterCockpit", "TEX00037" },
	{ "PreybirdFighterCockpit", "Canopy" },
	{ "TieAdvancedX1Cockpit", "Tex00029 Tex00067" },
	{ "TieAvengerCockpit", "Tex00022 Tex00023 Tex00075" },
	{ "TieBigGunCockpit", "Tex00011 Tex00060" },
	{ "TieBizarroCockpit", "Tex00011 Tex00060" },
	{ "TieBomberCockpit", "Tex00022 Tex00023" },
	{ "TieBoosterCockpit", "Tex00011 Tex00060" },
	{ "TIECockpit", "Tex00022 Tex00023" },
	{ "TieDefenderCockpit", "Tex00022 Tex00023" },
	{ "TieFighterCockpit", "Tex00022 Tex00023" },
	{ "TieFighterOutlandCockpit", "Tex00022 Tex00023" },
	{ "TieInterceptorCockpit", "Tex00022 Tex00023" },
	{ "TieWarheadsCockpit", "Tex00011 Tex00062" },
	{ "TwingCockpit", "Tex00025" },
	{ "XwingCockpit", "Tex00089" },
	{ "YwingCockpit", "TEX00018" },
	{ "Z-95Cockpit", "Tex00020" },
};

const char* ModelTables_LitTextures(const char* model) {
	return find_entry(kLitTextures, COUNT(kLitTextures), model);
}
