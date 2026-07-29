#include "internal.h"

#include <string.h>

void Aeron_BeginInputFrame(AeronInputSnapshot* input) {
	memset(input->key_pressed, 0, sizeof(input->key_pressed));
	memset(input->key_released, 0, sizeof(input->key_released));
	memset(input->key_typed, 0, sizeof(input->key_typed));
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
			memset(g_aeron.input.key_down, 0, sizeof(g_aeron.input.key_down));
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
				if (event->key.down) {
					if (!g_aeron.input.key_down[scancode] && !event->key.repeat) {
						g_aeron.input.key_pressed[scancode] = 1;
					}
					if (g_aeron.input.key_typed[scancode] < 255) {
						g_aeron.input.key_typed[scancode]++;
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
