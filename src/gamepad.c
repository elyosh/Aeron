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

static int Aeron_FindControllerSlot(SDL_JoystickID instance_id) {
	int slot;

	for (slot = 0; slot < AERON_CONTROLLER_MAX; ++slot) {
		if (g_aeron.controllers[slot].joystick != NULL &&
			g_aeron.controllers[slot].instance_id == instance_id) {
			return slot;
		}
	}

	return -1;
}

static int Aeron_FindFreeControllerSlot(void) {
	int slot;

	for (slot = 0; slot < AERON_CONTROLLER_MAX; ++slot) {
		if (g_aeron.controllers[slot].joystick == NULL) {
			return slot;
		}
	}

	return -1;
}

static void Aeron_ClearControllerSnapshot(int slot) {
	if (slot >= 0 && slot < AERON_CONTROLLER_MAX) {
		memset(&g_aeron.input.controllers[slot], 0, sizeof(g_aeron.input.controllers[slot]));
	}
}

static int Aeron_ControllerCount(int count, int maximum) {
	if (count < 0) {
		return 0;
	}
	return count < maximum ? count : maximum;
}

static int Aeron_ControllerHasRumbleHandle(SDL_Joystick* joystick) {
	SDL_PropertiesID props;

	if (!joystick) {
		return 0;
	}
	props = SDL_GetJoystickProperties(joystick);
	return props && SDL_GetBooleanProperty(props, SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false) ? 1 : 0;
}

static void Aeron_PopulateControllerIdentity(int slot) {
	AeronControllerDevice*   device;
	AeronControllerSnapshot* snapshot;
	SDL_GUID                 guid;
	const char*              name;
	const char*              path;
	int                      button;

	device   = &g_aeron.controllers[slot];
	snapshot = &g_aeron.input.controllers[slot];
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->connected   = 1;
	snapshot->kind        = device->gamepad ? AERON_CONTROLLER_KIND_GAMEPAD : AERON_CONTROLLER_KIND_JOYSTICK;
	snapshot->has_rumble  = device->has_rumble;
	snapshot->instance_id = device->instance_id;
	snapshot->axis_count  = (uint8_t)Aeron_ControllerCount(device->raw_axis_count, AERON_CONTROLLER_AXIS_MAX);
	snapshot->button_count =
		(uint8_t)Aeron_ControllerCount(device->raw_button_count, AERON_CONTROLLER_BUTTON_MAX);
	snapshot->hat_count = (uint8_t)Aeron_ControllerCount(device->raw_hat_count, AERON_CONTROLLER_HAT_MAX);
	snapshot->controls_truncated = device->raw_axis_count > AERON_CONTROLLER_AXIS_MAX ||
								   device->raw_button_count > AERON_CONTROLLER_BUTTON_MAX ||
								   device->raw_hat_count > AERON_CONTROLLER_HAT_MAX;
	for (button = 0; device->gamepad && button < AERON_GAMEPAD_BUTTON_COUNT; ++button) {
		if (SDL_GamepadHasButton(device->gamepad, (SDL_GamepadButton)button)) {
			snapshot->gamepad_available_buttons |= 1u << button;
		}
	}
	name                         = SDL_GetJoystickName(device->joystick);
	path                         = SDL_GetJoystickPath(device->joystick);
	Aeron_CopyString(snapshot->name, sizeof(snapshot->name), name ? name : "");
	Aeron_CopyString(snapshot->path, sizeof(snapshot->path), path ? path : "");
	guid = SDL_GetJoystickGUID(device->joystick);
	SDL_GUIDToString(guid, snapshot->guid, sizeof(snapshot->guid));
}

static int Aeron_OpenControllerInSlot(int slot, SDL_JoystickID instance_id) {
	AeronControllerDevice* device;
	SDL_Gamepad*           gamepad       = NULL;
	SDL_Joystick*          joystick      = NULL;
	int                    owns_joystick = 0;

	if (slot < 0 || slot >= AERON_CONTROLLER_MAX) {
		return 0;
	}

	if (SDL_IsGamepad(instance_id)) {
		gamepad = SDL_OpenGamepad(instance_id);
		if (gamepad) {
			joystick = SDL_GetGamepadJoystick(gamepad);
			if (!joystick) {
				Aeron_LogWarn("aeron.input", "SDL_GetGamepadJoystick(%u) failed: %s; trying raw joystick",
							  (unsigned int)instance_id, SDL_GetError());
				SDL_CloseGamepad(gamepad);
				gamepad = NULL;
			}
		} else {
			Aeron_LogWarn("aeron.input", "SDL_OpenGamepad(%u) failed: %s; trying raw joystick",
						  (unsigned int)instance_id, SDL_GetError());
		}
	}
	if (!joystick) {
		joystick      = SDL_OpenJoystick(instance_id);
		owns_joystick = joystick != NULL;
	}
	if (!joystick) {
		Aeron_LogWarn("aeron.input", "SDL_OpenJoystick(%u) failed: %s", (unsigned int)instance_id,
					  SDL_GetError());
		return 0;
	}

	device = &g_aeron.controllers[slot];
	memset(device, 0, sizeof(*device));
	device->gamepad          = gamepad;
	device->joystick         = joystick;
	device->instance_id      = instance_id;
	device->owns_joystick    = owns_joystick;
	device->raw_axis_count   = SDL_GetNumJoystickAxes(joystick);
	device->raw_button_count = SDL_GetNumJoystickButtons(joystick);
	device->raw_hat_count    = SDL_GetNumJoystickHats(joystick);
	device->has_rumble       = Aeron_ControllerHasRumbleHandle(joystick);
	Aeron_PopulateControllerIdentity(slot);
	Aeron_LogInfo("aeron.input",
				  "Opened controller slot %d id=%u kind=%s name='%s' axes=%d buttons=%d hats=%d rumble=%s",
				  slot, (unsigned int)instance_id, gamepad ? "gamepad" : "joystick",
				  g_aeron.input.controllers[slot].name, device->raw_axis_count, device->raw_button_count,
				  device->raw_hat_count, device->has_rumble ? "yes" : "no");
	if (g_aeron.input.controllers[slot].controls_truncated) {
		Aeron_LogWarn("aeron.input",
					  "Controller id=%u controls truncated to %d axes, %d buttons, and %d hats",
					  (unsigned int)instance_id, AERON_CONTROLLER_AXIS_MAX, AERON_CONTROLLER_BUTTON_MAX,
					  AERON_CONTROLLER_HAT_MAX);
	}
	return 1;
}

static void Aeron_OpenController(SDL_JoystickID instance_id) {
	int slot;

	if (Aeron_FindControllerSlot(instance_id) >= 0) {
		return;
	}
	slot = Aeron_FindFreeControllerSlot();
	if (slot < 0) {
		Aeron_LogWarn("aeron.input", "Ignoring controller %u: no free Aeron controller slot",
					  (unsigned int)instance_id);
		return;
	}
	Aeron_OpenControllerInSlot(slot, instance_id);
}

static void Aeron_CloseControllerSlot(int slot) {
	AeronControllerDevice* device;

	if (slot < 0 || slot >= AERON_CONTROLLER_MAX) {
		return;
	}
	device = &g_aeron.controllers[slot];
	if (!device->joystick) {
		return;
	}
	SDL_RumbleJoystick(device->joystick, 0, 0, 0);
	Aeron_LogInfo("aeron.input", "Closed controller slot %d id=%u", slot, (unsigned int)device->instance_id);
	if (device->gamepad) {
		SDL_CloseGamepad(device->gamepad);
	} else if (device->owns_joystick) {
		SDL_CloseJoystick(device->joystick);
	}
	memset(device, 0, sizeof(*device));
	Aeron_ClearControllerSnapshot(slot);
}

void Aeron_ControllersInit(void) {
	SDL_JoystickID* controllers;
	int             count;
	int             i;

	controllers = SDL_GetJoysticks(&count);
	if (!controllers) {
		return;
	}

	for (i = 0; i < count; ++i) {
		Aeron_OpenController(controllers[i]);
	}

	SDL_free(controllers);
}

void Aeron_ControllersShutdown(void) {
	int slot;

	for (slot = 0; slot < AERON_CONTROLLER_MAX; ++slot) {
		Aeron_CloseControllerSlot(slot);
	}
}

void Aeron_HandleControllerEvent(const SDL_Event* event) {
	int slot;

	if (!event) {
		return;
	}

	switch (event->type) {
		case SDL_EVENT_JOYSTICK_ADDED:
			Aeron_OpenController(event->jdevice.which);
			break;
		case SDL_EVENT_JOYSTICK_REMOVED:
			slot = Aeron_FindControllerSlot(event->jdevice.which);
			Aeron_CloseControllerSlot(slot);
			break;
		default:
			break;
	}
}

static void Aeron_UpdateControllerSnapshot(int slot, AeronControllerSnapshot* snapshot) {
	AeronControllerDevice* device;
	uint64_t               previous_raw_buttons;
	uint64_t               raw_buttons;
	uint32_t               previous_gamepad_buttons;
	uint32_t               gamepad_buttons;
	int                    i;

	device = &g_aeron.controllers[slot];
	if (!device->joystick) {
		Aeron_ClearControllerSnapshot(slot);
		return;
	}

	previous_raw_buttons = snapshot->raw_buttons;
	raw_buttons          = 0;
	for (i = 0; i < snapshot->axis_count; ++i) {
		snapshot->raw_axes[i] = SDL_GetJoystickAxis(device->joystick, i);
	}
	for (i = 0; i < snapshot->button_count; ++i) {
		if (SDL_GetJoystickButton(device->joystick, i)) {
			raw_buttons |= (uint64_t)1u << i;
		}
	}
	for (i = 0; i < snapshot->hat_count; ++i) {
		snapshot->raw_hats[i] = SDL_GetJoystickHat(device->joystick, i);
	}
	snapshot->raw_buttons          = raw_buttons;
	snapshot->raw_pressed_buttons  = raw_buttons & ~previous_raw_buttons;
	snapshot->raw_released_buttons = previous_raw_buttons & ~raw_buttons;

	previous_gamepad_buttons = snapshot->gamepad_buttons;
	gamepad_buttons          = 0;
	if (device->gamepad) {
		for (i = 0; i < AERON_GAMEPAD_AXIS_COUNT; ++i) {
			snapshot->gamepad_axes[i] = SDL_GetGamepadAxis(device->gamepad, (SDL_GamepadAxis)i);
		}
		for (i = 0; i < AERON_GAMEPAD_BUTTON_COUNT; ++i) {
			if (SDL_GetGamepadButton(device->gamepad, (SDL_GamepadButton)i)) {
				gamepad_buttons |= 1u << i;
			}
		}
	} else {
		memset(snapshot->gamepad_axes, 0, sizeof(snapshot->gamepad_axes));
	}
	snapshot->gamepad_buttons          = gamepad_buttons;
	snapshot->gamepad_pressed_buttons  = gamepad_buttons & ~previous_gamepad_buttons;
	snapshot->gamepad_released_buttons = previous_gamepad_buttons & ~gamepad_buttons;
}

void Aeron_UpdateControllers(AeronInputSnapshot* input) {
	int slot;

	if (!input) {
		return;
	}

	for (slot = 0; slot < AERON_CONTROLLER_MAX; ++slot) {
		Aeron_UpdateControllerSnapshot(slot, &input->controllers[slot]);
	}
}

int Aeron_ControllerHasRumble(uint32_t instance_id) {
	int slot = Aeron_FindControllerSlot((SDL_JoystickID)instance_id);
	return slot >= 0 ? g_aeron.controllers[slot].has_rumble : 0;
}

int Aeron_RumbleController(uint32_t instance_id, uint16_t low_frequency_rumble,
						   uint16_t high_frequency_rumble, uint32_t duration_ms) {
	int                    slot = Aeron_FindControllerSlot((SDL_JoystickID)instance_id);
	AeronControllerDevice* device;
	const int stopping = (low_frequency_rumble == 0 && high_frequency_rumble == 0) || duration_ms == 0;

	if (slot < 0) {
		if (!stopping) {
			Aeron_LogWarn("aeron.input", "Rumble requested for disconnected controller id=%u",
						  (unsigned int)instance_id);
		}
		return 0;
	}
	device = &g_aeron.controllers[slot];
	if (!device->joystick || !device->has_rumble) {
		if (!stopping) {
			Aeron_LogWarn("aeron.input", "Rumble requested for unsupported controller id=%u",
						  (unsigned int)instance_id);
		}
		return 0;
	}
	if (!SDL_RumbleJoystick(device->joystick, low_frequency_rumble, high_frequency_rumble,
							stopping ? 0 : duration_ms)) {
		if (!stopping) {
			Aeron_LogWarn("aeron.input", "Rumble failed for controller id=%u: %s", (unsigned int)instance_id,
						  SDL_GetError());
		}
		return 0;
	}
	return 1;
}
