#include "internal.h"

#include <string.h>

const char* AeronKey_Name(AeronKey key) {
	return key > 0 && key < AERON_KEY_COUNT ? SDL_GetScancodeName((SDL_Scancode)key) : "";
}

uint8_t AeronKey_Modifier(AeronKey key) {
	switch (key) {
		case AERON_KEY_LSHIFT:
		case AERON_KEY_RSHIFT:
			return AERON_KEY_MOD_SHIFT;
		case AERON_KEY_LCTRL:
		case AERON_KEY_RCTRL:
			return AERON_KEY_MOD_CTRL;
		case AERON_KEY_LALT:
		case AERON_KEY_RALT:
			return AERON_KEY_MOD_ALT;
		case AERON_KEY_LGUI:
		case AERON_KEY_RGUI:
			return AERON_KEY_MOD_GUI;
		default:
			return 0;
	}
}

static uint8_t Aeron_KeyModifiers(SDL_Keymod mod) {
	return ((mod & SDL_KMOD_SHIFT) ? AERON_KEY_MOD_SHIFT : 0) |
		   ((mod & SDL_KMOD_CTRL) ? AERON_KEY_MOD_CTRL : 0) | ((mod & SDL_KMOD_ALT) ? AERON_KEY_MOD_ALT : 0) |
		   ((mod & SDL_KMOD_GUI) ? AERON_KEY_MOD_GUI : 0);
}

int AeronKey_FromName(const char* name, AeronKey* out_key) {
	SDL_Scancode scancode;

	if (!name || !name[0] || !out_key) {
		return 0;
	}
	scancode = SDL_GetScancodeFromName(name);
	if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= AERON_KEY_COUNT) {
		return 0;
	}
	*out_key = (AeronKey)scancode;
	return 1;
}

void Aeron_BeginInputFrame(AeronInputSnapshot* input) {
	input->key_event_count     = 0;
	input->key_events_overflow = 0;
	memset(input->key_pressed, 0, sizeof(input->key_pressed));
	memset(input->key_released, 0, sizeof(input->key_released));
	memset(input->key_typed, 0, sizeof(input->key_typed));
	memset(input->key_alt_typed, 0, sizeof(input->key_alt_typed));
	input->mouse.relative_x             = 0.0f;
	input->mouse.relative_y             = 0.0f;
	input->mouse.wheel_x                = 0;
	input->mouse.wheel_y                = 0;
	input->mouse.pressed_buttons        = 0;
	input->mouse.released_buttons       = 0;
	input->mouse.double_clicked_buttons = 0;
	input->text[0]                      = '\0';
	input->text_length                  = 0;
	input->frame_id++;
}

static void Aeron_AppendText(AeronInputSnapshot* input, const char* text) {
	size_t available;
	size_t len;

	if (!text || !text[0] || input->text_length >= AERON_TEXT_INPUT_CAPACITY - 1) {
		return;
	}

	available = (size_t)AERON_TEXT_INPUT_CAPACITY - 1u - input->text_length;
	len       = strlen(text);
	if (len > available) {
		len = available;
	}

	memcpy(input->text + input->text_length, text, len);
	input->text_length += (uint32_t)len;
	input->text[input->text_length] = '\0';
}

static uint32_t Aeron_MouseButtonMask(uint8_t button) {
	switch (button) {
		case SDL_BUTTON_LEFT:
			return AERON_MOUSE_BUTTON_LEFT;
		case SDL_BUTTON_MIDDLE:
			return AERON_MOUSE_BUTTON_MIDDLE;
		case SDL_BUTTON_RIGHT:
			return AERON_MOUSE_BUTTON_RIGHT;
		case SDL_BUTTON_X1:
			return AERON_MOUSE_BUTTON_X1;
		case SDL_BUTTON_X2:
			return AERON_MOUSE_BUTTON_X2;
		default:
			if (button > 0 && button <= 32) {
				return 1u << (button - 1u);
			}
			return 0;
	}
}

static uint32_t Aeron_MouseButtonStateMask(uint32_t sdl_state) {
	uint32_t state = 0;

	if (sdl_state & SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) {
		state |= AERON_MOUSE_BUTTON_LEFT;
	}
	if (sdl_state & SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)) {
		state |= AERON_MOUSE_BUTTON_MIDDLE;
	}
	if (sdl_state & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) {
		state |= AERON_MOUSE_BUTTON_RIGHT;
	}
	if (sdl_state & SDL_BUTTON_MASK(SDL_BUTTON_X1)) {
		state |= AERON_MOUSE_BUTTON_X1;
	}
	if (sdl_state & SDL_BUTTON_MASK(SDL_BUTTON_X2)) {
		state |= AERON_MOUSE_BUTTON_X2;
	}

	return state;
}

static void Aeron_ReleaseAllKeys(AeronInputSnapshot* input) {
	int key;

	/* SDL queues its synthetic key-up events after the focus-lost event.
	 * Preserve the release edges before clearing level state so downstream
	 * buffered-input consumers cannot retain a held modifier. */
	for (key = 0; key < AERON_KEY_COUNT; ++key) {
		if (input->key_down[key]) {
			input->key_released[key] = 1;
		}
	}
	memset(input->key_down, 0, sizeof(input->key_down));
}

void Aeron_HandleEvent(const SDL_Event* event) {
	uint32_t mask;

	switch (event->type) {
		case SDL_EVENT_QUIT:
			g_aeron.quit_requested = 1;
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			g_aeron.input.has_focus = 1;
			break;
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			g_aeron.input.has_focus = 0;
			Aeron_ReleaseAllKeys(&g_aeron.input);
			g_aeron.input.mouse.buttons = 0;
			break;
		case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
			Aeron_RefreshPresentationTiming();
			/* The new display may have a different HDR mode and headroom. */
			Aeron_OnOutputHdrStateChanged();
			break;
		case SDL_EVENT_WINDOW_HDR_STATE_CHANGED:
			Aeron_OnOutputHdrStateChanged();
			break;
		case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
		case SDL_EVENT_WINDOW_RESTORED:
			Aeron_ApplyPendingWindowAspectRatio();
			break;
		case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
		case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
			if (event->display.displayID == g_aeron.presentation_display_id) {
				Aeron_RefreshPresentationTiming();
			}
			break;
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
			if (event->key.scancode >= 0 && event->key.scancode < AERON_KEY_COUNT) {
				const int scancode = event->key.scancode;
				if (g_aeron.input.key_event_count < AERON_KEY_EVENT_CAPACITY) {
					g_aeron.input.key_events[g_aeron.input.key_event_count++] = (AeronKeyEvent) {
						.chord  = { (uint16_t)scancode, Aeron_KeyModifiers(event->key.mod) },
						.down   = event->key.down,
						.repeat = event->key.repeat,
					};
				} else {
					g_aeron.input.key_events_overflow = 1;
				}
				if (event->key.down) {
					if (!g_aeron.input.key_down[scancode] && !event->key.repeat) {
						g_aeron.input.key_pressed[scancode] = 1;
					}
					if (g_aeron.input.key_typed[scancode] < 255) {
						g_aeron.input.key_typed[scancode]++;
					}
					if ((event->key.mod & SDL_KMOD_ALT) && g_aeron.input.key_alt_typed[scancode] < 255) {
						g_aeron.input.key_alt_typed[scancode]++;
					}
					g_aeron.input.key_down[scancode] = 1;
				} else {
					if (g_aeron.input.key_down[scancode]) {
						g_aeron.input.key_released[scancode] = 1;
					}
					g_aeron.input.key_down[scancode] = 0;
				}
			}
			break;
		case SDL_EVENT_TEXT_INPUT:
			Aeron_AppendText(&g_aeron.input, event->text.text);
			break;
		case SDL_EVENT_MOUSE_MOTION:
			g_aeron.input.mouse.raw_x = (int)event->motion.x;
			g_aeron.input.mouse.raw_y = (int)event->motion.y;
			g_aeron.input.mouse.relative_x += event->motion.xrel;
			g_aeron.input.mouse.relative_y += event->motion.yrel;
			g_aeron.input.mouse.buttons = Aeron_MouseButtonStateMask(event->motion.state);
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			mask                      = Aeron_MouseButtonMask(event->button.button);
			g_aeron.input.mouse.raw_x = (int)event->button.x;
			g_aeron.input.mouse.raw_y = (int)event->button.y;
			if (event->button.down) {
				g_aeron.input.mouse.buttons |= mask;
				g_aeron.input.mouse.pressed_buttons |= mask;
				if (event->button.clicks >= 2) {
					g_aeron.input.mouse.double_clicked_buttons |= mask;
				}
			} else {
				g_aeron.input.mouse.buttons &= ~mask;
				g_aeron.input.mouse.released_buttons |= mask;
			}
			break;
		case SDL_EVENT_MOUSE_WHEEL:
			g_aeron.input.mouse.wheel_x += event->wheel.integer_x;
			g_aeron.input.mouse.wheel_y += event->wheel.integer_y;
			break;
		case SDL_EVENT_JOYSTICK_ADDED:
		case SDL_EVENT_JOYSTICK_REMOVED:
			Aeron_HandleControllerEvent(event);
			break;
		default:
			break;
	}
}

const AeronInputSnapshot* Aeron_InputSnapshot(void) { return &g_aeron.input; }
