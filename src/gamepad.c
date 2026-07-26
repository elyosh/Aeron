#include "internal.h"

#include <string.h>

static const char* const g_aeronGamepadAxisNames[AERON_GAMEPAD_AXIS_COUNT] = {
	"leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger",
};

static const char* const g_aeronGamepadButtonNames[AERON_GAMEPAD_BUTTON_COUNT] = {
	"south",     "east",       "west",         "north",         "back",         "guide",       "start",
	"leftstick", "rightstick", "leftshoulder", "rightshoulder", "dpadup",       "dpaddown",    "dpadleft",
	"dpadright", "misc1",      "rightpaddle1", "leftpaddle1",   "rightpaddle2", "leftpaddle2", "touchpad",
	"misc2",     "misc3",      "misc4",        "misc5",         "misc6",
};

static int Aeron_GamepadNamesEqual(const char* a, const char* b) {
	if (!a || !b) {
		return 0;
	}

	while (*a && *b) {
		char ca = *a++;
		char cb = *b++;

		if (ca >= 'A' && ca <= 'Z') {
			ca = (char)(ca - 'A' + 'a');
		}
		if (cb >= 'A' && cb <= 'Z') {
			cb = (char)(cb - 'A' + 'a');
		}
		if (ca == '_' || ca == '-' || ca == ' ') {
			--b;
			continue;
		}
		if (cb == '_' || cb == '-' || cb == ' ') {
			--a;
			continue;
		}
		if (ca != cb) {
			return 0;
		}
	}

	while (*a == '_' || *a == '-' || *a == ' ') {
		++a;
	}
	while (*b == '_' || *b == '-' || *b == ' ') {
		++b;
	}

	return *a == '\0' && *b == '\0';
}

AeronGamepadAxis Aeron_GamepadAxisFromName(const char* name) {
	int i;

	if (Aeron_GamepadNamesEqual(name, "righttrigger")) {
		return AERON_GAMEPAD_AXIS_RIGHT_TRIGGER;
	}
	if (Aeron_GamepadNamesEqual(name, "lefttrigger")) {
		return AERON_GAMEPAD_AXIS_LEFT_TRIGGER;
	}

	for (i = 0; i < AERON_GAMEPAD_AXIS_COUNT; ++i) {
		if (Aeron_GamepadNamesEqual(name, g_aeronGamepadAxisNames[i])) {
			return (AeronGamepadAxis)i;
		}
	}

	return AERON_GAMEPAD_AXIS_COUNT;
}

AeronGamepadButton Aeron_GamepadButtonFromName(const char* name) {
	int i;

	for (i = 0; i < AERON_GAMEPAD_BUTTON_COUNT; ++i) {
		if (Aeron_GamepadNamesEqual(name, g_aeronGamepadButtonNames[i])) {
			return (AeronGamepadButton)i;
		}
	}

	return AERON_GAMEPAD_BUTTON_COUNT;
}

const char* Aeron_GamepadAxisName(AeronGamepadAxis axis) {
	if (axis < 0 || axis >= AERON_GAMEPAD_AXIS_COUNT) {
		return NULL;
	}

	return g_aeronGamepadAxisNames[axis];
}

const char* Aeron_GamepadButtonName(AeronGamepadButton button) {
	if (button < 0 || button >= AERON_GAMEPAD_BUTTON_COUNT) {
		return NULL;
	}

	return g_aeronGamepadButtonNames[button];
}

static int Aeron_FindGamepadSlot(SDL_JoystickID instance_id) {
	int slot;

	for (slot = 0; slot < AERON_GAMEPAD_MAX; ++slot) {
		if (g_aeron.gamepads[slot].gamepad != NULL && g_aeron.gamepads[slot].instance_id == instance_id) {
			return slot;
		}
	}

	return -1;
}

static int Aeron_FindFreeGamepadSlot(void) {
	int slot;

	for (slot = 0; slot < AERON_GAMEPAD_MAX; ++slot) {
		if (g_aeron.gamepads[slot].gamepad == NULL) {
			return slot;
		}
	}

	return -1;
}

static void Aeron_ClearGamepadSnapshot(int slot) {
	if (slot >= 0 && slot < AERON_GAMEPAD_MAX) {
		memset(&g_aeron.input.gamepads[slot], 0, sizeof(g_aeron.input.gamepads[slot]));
	}
}

static void Aeron_OpenGamepad(SDL_JoystickID instance_id) {
	int          slot;
	SDL_Gamepad* gamepad;

	if (Aeron_FindGamepadSlot(instance_id) >= 0) {
		return;
	}

	slot = Aeron_FindFreeGamepadSlot();
	if (slot < 0) {
		Aeron_Log("aeron.input", "Ignoring gamepad %u: no free Aeron gamepad slot",
				  (unsigned int)instance_id);
		return;
	}

	gamepad = SDL_OpenGamepad(instance_id);
	if (!gamepad) {
		Aeron_Log("aeron.input", "SDL_OpenGamepad(%u) failed: %s", (unsigned int)instance_id, SDL_GetError());
		return;
	}

	g_aeron.gamepads[slot].gamepad     = gamepad;
	g_aeron.gamepads[slot].instance_id = instance_id;
	Aeron_Log("aeron.input", "Opened gamepad slot %d id=%u name='%s'", slot, (unsigned int)instance_id,
			  SDL_GetGamepadName(gamepad));
}

static void Aeron_CloseGamepadSlot(int slot) {
	if (slot < 0 || slot >= AERON_GAMEPAD_MAX || g_aeron.gamepads[slot].gamepad == NULL) {
		return;
	}

	SDL_CloseGamepad(g_aeron.gamepads[slot].gamepad);
	g_aeron.gamepads[slot].gamepad     = NULL;
	g_aeron.gamepads[slot].instance_id = 0;
	Aeron_ClearGamepadSnapshot(slot);
}

void Aeron_GamepadsInit(void) {
	SDL_JoystickID* gamepads;
	int             count;
	int             i;

	gamepads = SDL_GetGamepads(&count);
	if (!gamepads) {
		return;
	}

	for (i = 0; i < count; ++i) {
		Aeron_OpenGamepad(gamepads[i]);
	}

	SDL_free(gamepads);
}

void Aeron_GamepadsShutdown(void) {
	int slot;

	for (slot = 0; slot < AERON_GAMEPAD_MAX; ++slot) {
		Aeron_CloseGamepadSlot(slot);
	}
}

void Aeron_HandleGamepadEvent(const SDL_Event* event) {
	int slot;

	if (!event) {
		return;
	}

	switch (event->type) {
		case SDL_EVENT_GAMEPAD_ADDED:
			Aeron_OpenGamepad(event->gdevice.which);
			break;
		case SDL_EVENT_GAMEPAD_REMOVED:
			slot = Aeron_FindGamepadSlot(event->gdevice.which);
			Aeron_CloseGamepadSlot(slot);
			break;
		case SDL_EVENT_GAMEPAD_REMAPPED:
			slot = Aeron_FindGamepadSlot(event->gdevice.which);
			if (slot >= 0) {
				Aeron_Log("aeron.input", "Gamepad slot %d remapped", slot);
			}
			break;
		default:
			break;
	}
}

static void Aeron_UpdateGamepadSnapshot(int slot, AeronGamepadSnapshot* snapshot) {
	SDL_Gamepad*   gamepad;
	SDL_JoystickID instance_id;
	SDL_GUID       guid;
	const char*    name;
	const char*    path;
	uint32_t       previous_buttons;
	uint32_t       buttons;
	int            i;

	gamepad = g_aeron.gamepads[slot].gamepad;
	if (!gamepad) {
		Aeron_ClearGamepadSnapshot(slot);
		return;
	}

	instance_id      = g_aeron.gamepads[slot].instance_id;
	previous_buttons = snapshot->buttons;
	buttons          = 0;
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->connected   = 1;
	snapshot->instance_id = instance_id;

	name = SDL_GetGamepadName(gamepad);
	path = SDL_GetGamepadPath(gamepad);
	Aeron_CopyString(snapshot->name, sizeof(snapshot->name), name ? name : "");
	Aeron_CopyString(snapshot->path, sizeof(snapshot->path), path ? path : "");
	guid = SDL_GetGamepadGUIDForID(instance_id);
	SDL_GUIDToString(guid, snapshot->guid, sizeof(snapshot->guid));

	for (i = 0; i < AERON_GAMEPAD_AXIS_COUNT; ++i) {
		snapshot->axes[i] = SDL_GetGamepadAxis(gamepad, (SDL_GamepadAxis)i);
	}

	for (i = 0; i < AERON_GAMEPAD_BUTTON_COUNT; ++i) {
		if (SDL_GetGamepadButton(gamepad, (SDL_GamepadButton)i)) {
			buttons |= 1u << i;
		}
	}

	snapshot->buttons          = buttons;
	snapshot->pressed_buttons  = buttons & ~previous_buttons;
	snapshot->released_buttons = previous_buttons & ~buttons;
}

void Aeron_UpdateGamepads(AeronInputSnapshot* input) {
	int slot;

	if (!input) {
		return;
	}

	for (slot = 0; slot < AERON_GAMEPAD_MAX; ++slot) {
		Aeron_UpdateGamepadSnapshot(slot, &input->gamepads[slot]);
	}
}

int Aeron_GamepadHasRumble(int slot) {
	SDL_Gamepad*     gamepad;
	SDL_PropertiesID props;

	if (slot < 0 || slot >= AERON_GAMEPAD_MAX) {
		return 0;
	}
	gamepad = g_aeron.gamepads[slot].gamepad;
	if (!gamepad) {
		return 0;
	}
	props = SDL_GetGamepadProperties(gamepad);
	if (props == 0) {
		return 0;
	}
	return SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? 1 : 0;
}

int Aeron_RumbleGamepad(int slot, uint16_t low_frequency_rumble, uint16_t high_frequency_rumble,
						uint32_t duration_ms) {
	SDL_Gamepad* gamepad;

	if (slot < 0 || slot >= AERON_GAMEPAD_MAX) {
		return 0;
	}
	gamepad = g_aeron.gamepads[slot].gamepad;
	if (!gamepad) {
		return 0;
	}
	return SDL_RumbleGamepad(gamepad, low_frequency_rumble, high_frequency_rumble, duration_ms) ? 1 : 0;
}
