#ifndef AERON_INPUT_H
#define AERON_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AERON_KEY_COUNT 512
#define AERON_TEXT_INPUT_CAPACITY 1024
#define AERON_CONTROLLER_MAX 4
#define AERON_CONTROLLER_AXIS_MAX 16
#define AERON_CONTROLLER_BUTTON_MAX 64
#define AERON_CONTROLLER_HAT_MAX 4
#define AERON_CONTROLLER_NAME_CAPACITY 128
#define AERON_CONTROLLER_PATH_CAPACITY 256

/* SDL scancode-compatible key identifiers exposed by Aeron. */
typedef enum AeronKey {
	AERON_KEY_A            = 4,
	AERON_KEY_1            = 30,
	AERON_KEY_RETURN       = 40,
	AERON_KEY_ESCAPE       = 41,
	AERON_KEY_BACKSPACE    = 42,
	AERON_KEY_TAB          = 43,
	AERON_KEY_SPACE        = 44,
	AERON_KEY_MINUS        = 45,
	AERON_KEY_EQUALS       = 46,
	AERON_KEY_LEFTBRACKET  = 47,
	AERON_KEY_RIGHTBRACKET = 48,
	AERON_KEY_BACKSLASH    = 49,
	AERON_KEY_SEMICOLON    = 51,
	AERON_KEY_APOSTROPHE   = 52,
	AERON_KEY_GRAVE        = 53,
	AERON_KEY_COMMA        = 54,
	AERON_KEY_PERIOD       = 55,
	AERON_KEY_SLASH        = 56,
	AERON_KEY_CAPSLOCK     = 57,
	AERON_KEY_F1           = 58,
	AERON_KEY_F11          = 68,
	AERON_KEY_PRINTSCREEN  = 70,
	AERON_KEY_SCROLLLOCK   = 71,
	AERON_KEY_PAUSE        = 72,
	AERON_KEY_INSERT       = 73,
	AERON_KEY_HOME         = 74,
	AERON_KEY_PAGEUP       = 75,
	AERON_KEY_DELETE       = 76,
	AERON_KEY_END          = 77,
	AERON_KEY_PAGEDOWN     = 78,
	AERON_KEY_RIGHT        = 79,
	AERON_KEY_LEFT         = 80,
	AERON_KEY_DOWN         = 81,
	AERON_KEY_UP           = 82,
	AERON_KEY_KP_DIVIDE    = 84,
	AERON_KEY_KP_MULTIPLY  = 85,
	AERON_KEY_KP_MINUS     = 86,
	AERON_KEY_KP_PLUS      = 87,
	AERON_KEY_KP_ENTER     = 88,
	AERON_KEY_KP_1         = 89,
	AERON_KEY_KP_2         = 90,
	AERON_KEY_KP_3         = 91,
	AERON_KEY_KP_4         = 92,
	AERON_KEY_KP_5         = 93,
	AERON_KEY_KP_6         = 94,
	AERON_KEY_KP_7         = 95,
	AERON_KEY_KP_8         = 96,
	AERON_KEY_KP_9         = 97,
	AERON_KEY_KP_0         = 98,
	AERON_KEY_KP_PERIOD    = 99,
	AERON_KEY_LCTRL        = 224,
	AERON_KEY_LSHIFT       = 225,
	AERON_KEY_LALT         = 226,
	AERON_KEY_LGUI         = 227,
	AERON_KEY_RCTRL        = 228,
	AERON_KEY_RSHIFT       = 229,
	AERON_KEY_RALT         = 230,
	AERON_KEY_RGUI         = 231
} AeronKey;

/* Bit flags used for mouse button state and button-edge snapshots. */
typedef enum AeronMouseButton {
	AERON_MOUSE_BUTTON_LEFT   = 1u << 0,
	AERON_MOUSE_BUTTON_MIDDLE = 1u << 1,
	AERON_MOUSE_BUTTON_RIGHT  = 1u << 2,
	AERON_MOUSE_BUTTON_X1     = 1u << 3,
	AERON_MOUSE_BUTTON_X2     = 1u << 4
} AeronMouseButton;

/* Standard SDL Gamepad API axes, exposed without game-specific semantics. */
typedef enum AeronGamepadAxis {
	AERON_GAMEPAD_AXIS_LEFTX = 0,
	AERON_GAMEPAD_AXIS_LEFTY,
	AERON_GAMEPAD_AXIS_RIGHTX,
	AERON_GAMEPAD_AXIS_RIGHTY,
	AERON_GAMEPAD_AXIS_LEFT_TRIGGER,
	AERON_GAMEPAD_AXIS_RIGHT_TRIGGER,
	AERON_GAMEPAD_AXIS_COUNT
} AeronGamepadAxis;

/* Standard SDL Gamepad API buttons, exposed without game-specific semantics. */
typedef enum AeronGamepadButton {
	AERON_GAMEPAD_BUTTON_SOUTH = 0,
	AERON_GAMEPAD_BUTTON_EAST,
	AERON_GAMEPAD_BUTTON_WEST,
	AERON_GAMEPAD_BUTTON_NORTH,
	AERON_GAMEPAD_BUTTON_BACK,
	AERON_GAMEPAD_BUTTON_GUIDE,
	AERON_GAMEPAD_BUTTON_START,
	AERON_GAMEPAD_BUTTON_LEFT_STICK,
	AERON_GAMEPAD_BUTTON_RIGHT_STICK,
	AERON_GAMEPAD_BUTTON_LEFT_SHOULDER,
	AERON_GAMEPAD_BUTTON_RIGHT_SHOULDER,
	AERON_GAMEPAD_BUTTON_DPAD_UP,
	AERON_GAMEPAD_BUTTON_DPAD_DOWN,
	AERON_GAMEPAD_BUTTON_DPAD_LEFT,
	AERON_GAMEPAD_BUTTON_DPAD_RIGHT,
	AERON_GAMEPAD_BUTTON_MISC1,
	AERON_GAMEPAD_BUTTON_RIGHT_PADDLE1,
	AERON_GAMEPAD_BUTTON_LEFT_PADDLE1,
	AERON_GAMEPAD_BUTTON_RIGHT_PADDLE2,
	AERON_GAMEPAD_BUTTON_LEFT_PADDLE2,
	AERON_GAMEPAD_BUTTON_TOUCHPAD,
	AERON_GAMEPAD_BUTTON_MISC2,
	AERON_GAMEPAD_BUTTON_MISC3,
	AERON_GAMEPAD_BUTTON_MISC4,
	AERON_GAMEPAD_BUTTON_MISC5,
	AERON_GAMEPAD_BUTTON_MISC6,
	AERON_GAMEPAD_BUTTON_COUNT
} AeronGamepadButton;

/*
 * Mouse state for the current frame; x/y are the last valid logical content
 * coordinates, raw_x/raw_y are platform window coordinates, and inside_content
 * reports whether x/y were refreshed this frame.
 */
typedef struct AeronMouseSnapshot {
	int      x;
	int      y;
	int      raw_x;
	int      raw_y;
	/* Raw pointer deltas this frame in window points, event-accumulated
	 * at float precision (HiDPI mice / trackpads emit fractional
	 * per-event deltas that vanish under int truncation). Always the
	 * raw SDL motion deltas, in both pointer modes — consumers wanting
	 * logical-space position deltas diff x/y across frames instead.
	 * Integer consumers must carry the fraction across frames
	 * (accumulate float, floor, keep the remainder). */
	float    relative_x;
	float    relative_y;
	int      wheel_x;
	int      wheel_y;
	int      inside_content;
	uint32_t buttons;
	uint32_t pressed_buttons;
	uint32_t released_buttons;
	uint32_t double_clicked_buttons;
} AeronMouseSnapshot;

typedef enum AeronControllerKind {
	AERON_CONTROLLER_KIND_NONE = 0,
	AERON_CONTROLLER_KIND_JOYSTICK,
	AERON_CONTROLLER_KIND_GAMEPAD,
} AeronControllerKind;

/* SDL-compatible joystick hat bits. Diagonals are bitwise combinations. */
typedef enum AeronControllerHat {
	AERON_CONTROLLER_HAT_CENTERED = 0,
	AERON_CONTROLLER_HAT_UP       = 1u << 0,
	AERON_CONTROLLER_HAT_RIGHT    = 1u << 1,
	AERON_CONTROLLER_HAT_DOWN     = 1u << 2,
	AERON_CONTROLLER_HAT_LEFT     = 1u << 3,
} AeronControllerHat;

/*
 * State for one connected SDL controller. Raw axis values use SDL's signed
 * 16-bit joystick range. When kind is GAMEPAD, gamepad_axes/buttons also
 * expose SDL's standardized layout; trigger axes are 0..32767.
 */
typedef struct AeronControllerSnapshot {
	int                 connected;
	AeronControllerKind kind;
	int                 has_rumble;
	uint32_t            instance_id;
	char                name[AERON_CONTROLLER_NAME_CAPACITY];
	char                path[AERON_CONTROLLER_PATH_CAPACITY];
	char                guid[33];
	uint8_t             axis_count;
	uint8_t             button_count;
	uint8_t             hat_count;
	uint8_t             controls_truncated;
	int16_t             raw_axes[AERON_CONTROLLER_AXIS_MAX];
	uint64_t            raw_buttons;
	uint64_t            raw_pressed_buttons;
	uint64_t            raw_released_buttons;
	uint8_t             raw_hats[AERON_CONTROLLER_HAT_MAX];
	int16_t             gamepad_axes[AERON_GAMEPAD_AXIS_COUNT];
	/* Standardized axes physically exposed by this gamepad. */
	uint32_t            gamepad_available_axes;
	/* Standardized buttons physically exposed by this gamepad. */
	uint32_t            gamepad_available_buttons;
	uint32_t            gamepad_buttons;
	uint32_t            gamepad_pressed_buttons;
	uint32_t            gamepad_released_buttons;
} AeronControllerSnapshot;

/* Complete keyboard, text, mouse, controller, focus, and window-size state captured by Aeron_BeginFrame. */
typedef struct AeronInputSnapshot {
	uint64_t             frame_id;
	uint8_t              key_down[AERON_KEY_COUNT];
	uint8_t              key_pressed[AERON_KEY_COUNT];
	uint8_t              key_released[AERON_KEY_COUNT];
	/* Key-down event count this frame INCLUDING OS typematic repeats —
	 * key_pressed reports only the initial edge. Consumers emulating DOS
	 * BIOS-style keyboard queues count repeats through this. */
	uint8_t                 key_typed[AERON_KEY_COUNT];
	AeronMouseSnapshot      mouse;
	AeronControllerSnapshot controllers[AERON_CONTROLLER_MAX];
	char                    text[AERON_TEXT_INPUT_CAPACITY];
	uint32_t                text_length;
	int                     window_width;
	int                     window_height;
	int                     has_focus;
} AeronInputSnapshot;

/* Parses a standard SDL gamepad axis name such as "leftx" or "righttrigger". */
AeronGamepadAxis Aeron_GamepadAxisFromName(const char* name);
/* Parses a standard SDL gamepad button name such as "south" or "dpaddown". */
AeronGamepadButton Aeron_GamepadButtonFromName(const char* name);
const char*        Aeron_GamepadAxisName(AeronGamepadAxis axis);
const char*        Aeron_GamepadButtonName(AeronGamepadButton button);

/* Two-motor rumble output for the connected controller with this runtime SDL
 * instance ID. Instance IDs are transient and must not be persisted.
 * low_frequency_rumble / high_frequency_rumble are 0..0xFFFF motor magnitudes;
 * duration_ms bounds the effect (SDL stops it automatically, 0 = stop now).
 * Returns non-zero on success, 0 if the controller is absent or unsupported. */
int Aeron_RumbleController(uint32_t instance_id, uint16_t low_frequency_rumble,
						   uint16_t high_frequency_rumble, uint32_t duration_ms);
/* Non-zero if the connected controller reports rumble capability. */
int Aeron_ControllerHasRumble(uint32_t instance_id);

#ifdef __cplusplus
}
#endif

#endif
