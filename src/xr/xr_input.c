/* The two VR hand controllers.
 *
 * The first part of this file is in every build. It keeps the state of the
 * two hands, fills their entries of the input snapshot and gives them to
 * Aeron_XrHands. The hands come from the headset, or from
 * Aeron_XrSetFakeHands when the device has no VR session.
 *
 * The second part reads the headset's controllers through OpenXR actions.
 * There is one action for each physical input, such as the trigger, and not
 * one for each thing a game does: the game binds the inputs itself. */

#include "../internal.h"
#include "xr_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* A click derived from a trigger or squeeze value is pressed at this value
 * and released a little lower. */
#define AERON_VR_HAND_CLICK_DOWN 0.7f
#define AERON_VR_HAND_CLICK_UP 0.6f
/* One haptic call lasts at most this long. A longer effect is sent again
 * before the call ends. */
#define AERON_VR_HAND_HAPTIC_US 500000u
#define AERON_VR_HAND_HAPTIC_RENEW_US 400000u

typedef struct AeronHandState {
	/* The controller is on. */
	int connected;
	/* The headset knows where the controller is: the two poses are valid. */
	int           tracked;
	AeronViewPose grip;
	AeronViewPose aim;
	/* From 0 to 1 for the trigger and the squeeze, from -1 to 1 for the
	 * others. */
	float    axes[AERON_VR_HAND_AXIS_COUNT];
	uint32_t buttons;
} AeronHandState;

typedef struct AeronHandHaptic {
	float    amplitude; /* 0 when the hand doesn't vibrate */
	uint64_t end_us;
	uint64_t renew_us; /* when to send the effect again */
} AeronHandHaptic;

static struct {
	AeronHandState  hands[2];
	AeronXrFakeHand fake[2];
	int             has_fake;
	AeronHandHaptic haptics[2];
	/* AERON_XR_INPUT_LOG is set: every change of an input is logged. */
	int      log_inputs;
	int      log_inputs_read;
	int16_t  logged_axes[2][AERON_VR_HAND_AXIS_COUNT];
	uint64_t pose_logged_us;
} g_hands;

static const char* const g_handNames[2] = { "left", "right" };
static const char* const g_handGuids[2] = {
	/* "aeronvrhandleft" and "aeronvrhandright" in hexadecimal. */
	"6165726f6e767268616e646c65667400",
	"6165726f6e767268616e647269676874",
};

#if AERON_XR
static void xr_read_hands(AeronHandState hands[2]);
static void xr_apply_haptic(int hand, float amplitude, uint64_t duration_us);
#endif

const char* Aeron_VrHandAxisName(int hand, int axis) {
	static const char* const names[AERON_VR_HAND_AXIS_COUNT] = {
		"Trigger", "Squeeze", "Thumbstick X", "Thumbstick Y", "Trackpad X", "Trackpad Y",
	};
	(void)hand;
	return axis >= 0 && axis < AERON_VR_HAND_AXIS_COUNT ? names[axis] : NULL;
}

const char* Aeron_VrHandButtonName(int hand, int button) {
	static const char* const names[AERON_VR_HAND_BUTTON_COUNT] = {
		"Trigger Click", "Squeeze Click", NULL, NULL, "Thumbstick Click", "Trackpad Click", "Menu",
	};
	if (button < 0 || button >= AERON_VR_HAND_BUTTON_COUNT)
		return NULL;
	if (button == AERON_VR_HAND_BUTTON_PRIMARY)
		return hand == 0 ? "X" : "A";
	if (button == AERON_VR_HAND_BUTTON_SECONDARY)
		return hand == 0 ? "Y" : "B";
	return names[button];
}

int AeronXr_HandFromInstance(uint32_t instance_id) {
	if (instance_id == AERON_VR_HAND_INSTANCE_LEFT)
		return 0;
	if (instance_id == AERON_VR_HAND_INSTANCE_RIGHT)
		return 1;
	return -1;
}

void Aeron_XrSetFakeHands(const AeronXrFakeHand hands[2]) {
	g_hands.has_fake = hands != NULL;
	if (hands)
		memcpy(g_hands.fake, hands, sizeof g_hands.fake);
	else
		SDL_zero(g_hands.fake);
}

int Aeron_XrHands(AeronXrHand out[2]) {
	int any = 0;
	for (int hand = 0; hand < 2; hand++) {
		const AeronHandState* state = &g_hands.hands[hand];
		SDL_zero(out[hand]);
		out[hand].active = state->connected && state->tracked;
		if (!out[hand].active)
			continue;
		out[hand].grip    = state->grip;
		out[hand].aim     = state->aim;
		out[hand].trigger = state->axes[AERON_VR_HAND_AXIS_TRIGGER];
		out[hand].squeeze = state->axes[AERON_VR_HAND_AXIS_SQUEEZE];
		any               = 1;
	}
	return any;
}

static void hand_from_fake(const AeronXrFakeHand* fake, AeronHandState* state) {
	SDL_zero(*state);
	if (!fake->active)
		return;
	state->connected = 1;
	state->tracked   = 1;
	state->grip      = fake->pose;
	state->aim       = fake->pose;
	state->buttons   = fake->buttons & ((1u << AERON_VR_HAND_BUTTON_COUNT) - 1u);
	if (state->buttons & (1u << AERON_VR_HAND_BUTTON_TRIGGER))
		state->axes[AERON_VR_HAND_AXIS_TRIGGER] = 1.0f;
	if (state->buttons & (1u << AERON_VR_HAND_BUTTON_SQUEEZE))
		state->axes[AERON_VR_HAND_AXIS_SQUEEZE] = 1.0f;
}

static int16_t hand_axis_to_raw(float value) {
	const float scaled = value * 32767.0f;
	if (scaled >= 32767.0f)
		return 32767;
	if (scaled <= -32768.0f)
		return -32768;
	return (int16_t)lroundf(scaled);
}

/* Logs what changed on a hand since the last frame. An axis is logged when
 * it moved by a quarter of its range since its last log line, and when it
 * comes back to rest. */
static void hand_log_inputs(int hand, const AeronControllerSnapshot* snapshot) {
	for (int button = 0; button < AERON_VR_HAND_BUTTON_COUNT; button++) {
		const int pressed  = Aeron_ControllerButtonBit(snapshot->raw_pressed_buttons, button);
		const int released = Aeron_ControllerButtonBit(snapshot->raw_released_buttons, button);
		if (pressed || released)
			Aeron_LogInfo("aeron.xr", "%s hand: %s %s", g_handNames[hand],
						  Aeron_VrHandButtonName(hand, button), pressed ? "pressed" : "released");
	}
	for (int axis = 0; axis < AERON_VR_HAND_AXIS_COUNT; axis++) {
		const int value  = snapshot->raw_axes[axis];
		const int logged = g_hands.logged_axes[hand][axis];
		if (value == logged || (abs(value - logged) < 8192 && value != 0))
			continue;
		g_hands.logged_axes[hand][axis] = (int16_t)value;
		Aeron_LogInfo("aeron.xr", "%s hand: %s %.2f", g_handNames[hand], Aeron_VrHandAxisName(hand, axis),
					  (double)value / 32767.0);
	}
}

/* Converts a pose's orientation to a row-major 3x3 matrix, which rotates
 * from the pose's axes to the rig's axes. */
static void hand_pose_rotation(const AeronViewPose* pose, float out[9]) {
	const float x = pose->orientation[0], y = pose->orientation[1];
	const float z = pose->orientation[2], w = pose->orientation[3];
	out[0] = 1.0f - 2.0f * (y * y + z * z);
	out[1] = 2.0f * (x * y - w * z);
	out[2] = 2.0f * (x * z + w * y);
	out[3] = 2.0f * (x * y + w * z);
	out[4] = 1.0f - 2.0f * (x * x + z * z);
	out[5] = 2.0f * (y * z - w * x);
	out[6] = 2.0f * (x * z - w * y);
	out[7] = 2.0f * (y * z + w * x);
	out[8] = 1.0f - 2.0f * (x * x + y * y);
}

/* The direction a pose points in: its -Z axis, in the rig's space. */
static void hand_pose_forward(const AeronViewPose* pose, float out[3]) {
	const float x = pose->orientation[0], y = pose->orientation[1];
	const float z = pose->orientation[2], w = pose->orientation[3];
	out[0] = -2.0f * (x * z + w * y);
	out[1] = -2.0f * (y * z - w * x);
	out[2] = -(1.0f - 2.0f * (x * x + y * y));
}

/* Logs where the hands are, once a second. */
static void hand_log_poses(void) {
	const uint64_t now = Aeron_NowUs();
	if (now - g_hands.pose_logged_us < 1000000u)
		return;
	g_hands.pose_logged_us = now;
	for (int hand = 0; hand < 2; hand++) {
		const AeronHandState* state = &g_hands.hands[hand];
		float                 grip[3], aim[3];
		if (!state->connected)
			continue;
		if (!state->tracked) {
			Aeron_LogInfo("aeron.xr", "%s hand: not tracked", g_handNames[hand]);
			continue;
		}
		hand_pose_forward(&state->grip, grip);
		hand_pose_forward(&state->aim, aim);
		Aeron_LogInfo("aeron.xr",
					  "%s hand: grip at (%.2f, %.2f, %.2f) m pointing (%.2f, %.2f, %.2f), aim at "
					  "(%.2f, %.2f, %.2f) m pointing (%.2f, %.2f, %.2f)",
					  g_handNames[hand], state->grip.position_m[0], state->grip.position_m[1],
					  state->grip.position_m[2], grip[0], grip[1], grip[2], state->aim.position_m[0],
					  state->aim.position_m[1], state->aim.position_m[2], aim[0], aim[1], aim[2]);
	}
}

static void hand_fill_snapshot(int hand, const AeronHandState* state, AeronControllerSnapshot* snapshot) {
	uint64_t buttons;

	if (!state->connected) {
		if (snapshot->connected) {
			Aeron_LogInfo("aeron.xr", "the %s controller is off", g_handNames[hand]);
			memset(snapshot, 0, sizeof *snapshot);
			SDL_zero(g_hands.logged_axes[hand]);
		}
		return;
	}
	if (!snapshot->connected) {
		memset(snapshot, 0, sizeof *snapshot);
		snapshot->connected    = 1;
		snapshot->kind         = AERON_CONTROLLER_KIND_VR_HAND;
		snapshot->has_rumble   = 1;
		snapshot->instance_id  = hand == 0 ? AERON_VR_HAND_INSTANCE_LEFT : AERON_VR_HAND_INSTANCE_RIGHT;
		snapshot->axis_count   = AERON_VR_HAND_AXIS_COUNT;
		snapshot->button_count = AERON_VR_HAND_BUTTON_COUNT;
		Aeron_CopyString(snapshot->name, sizeof snapshot->name,
						 hand == 0 ? "Left VR Controller" : "Right VR Controller");
		Aeron_CopyString(snapshot->guid, sizeof snapshot->guid, g_handGuids[hand]);
		Aeron_LogInfo("aeron.xr", "the %s controller is on", g_handNames[hand]);
	}
	for (int axis = 0; axis < AERON_VR_HAND_AXIS_COUNT; axis++)
		snapshot->raw_axes[axis] = hand_axis_to_raw(state->axes[axis]);
	buttons                           = state->buttons;
	snapshot->raw_pressed_buttons[0]  = buttons & ~snapshot->raw_buttons[0];
	snapshot->raw_released_buttons[0] = snapshot->raw_buttons[0] & ~buttons;
	snapshot->raw_buttons[0]          = buttons;
	if (g_hands.log_inputs)
		hand_log_inputs(hand, snapshot);
}

/* Keeps a hand vibrating for as long as the last rumble call asked. */
static void hand_update_haptic(int hand, uint64_t now) {
	AeronHandHaptic* haptic = &g_hands.haptics[hand];
	if (haptic->amplitude <= 0.0f)
		return;
	if (!g_hands.hands[hand].connected || now >= haptic->end_us) {
		haptic->amplitude = 0.0f;
		return;
	}
	if (now < haptic->renew_us)
		return;
	const uint64_t left     = haptic->end_us - now;
	const uint64_t duration = left < AERON_VR_HAND_HAPTIC_US ? left : AERON_VR_HAND_HAPTIC_US;
	/* The last call of an effect isn't renewed. */
	haptic->renew_us = left > AERON_VR_HAND_HAPTIC_US ? now + AERON_VR_HAND_HAPTIC_RENEW_US : UINT64_MAX;
#if AERON_XR
	xr_apply_haptic(hand, haptic->amplitude, duration);
#else
	(void)duration;
#endif
}

void AeronXr_UpdateHands(AeronInputSnapshot* input) {
	if (!g_hands.log_inputs_read) {
		const char* value       = SDL_getenv("AERON_XR_INPUT_LOG");
		g_hands.log_inputs      = value && value[0] && strcmp(value, "0") != 0;
		g_hands.log_inputs_read = 1;
	}

	AeronHandState hands[2];
	SDL_zero(hands);
	if (Aeron_XrEnabled()) {
#if AERON_XR
		xr_read_hands(hands);
#endif
	} else if (g_hands.has_fake) {
		for (int hand = 0; hand < 2; hand++)
			hand_from_fake(&g_hands.fake[hand], &hands[hand]);
	}

	const uint64_t now = Aeron_NowUs();
	for (int hand = 0; hand < 2; hand++) {
		g_hands.hands[hand] = hands[hand];
		hand_fill_snapshot(hand, &hands[hand], &input->controllers[AERON_CONTROLLER_SDL_MAX + hand]);
		hand_update_haptic(hand, now);
	}
	if (g_hands.log_inputs)
		hand_log_poses();
}

/* The pointer on the virtual screen.
 *
 * A hand that points at the screen moves the mouse of the input snapshot:
 * the trigger is the left button, the squeeze is the right button and the
 * thumbstick scrolls. The frontend and the UI then work as with a mouse. */

/* Two presses this close in time and in place make a double click. */
#define AERON_XR_POINTER_DOUBLE_CLICK_US 400000u
/* After a press, the pointer stays still this long, so that pulling the
 * trigger doesn't move it off a small target. */
#define AERON_XR_POINTER_PRESS_HOLD_US 100000u
/* The thumbstick scrolls one step each time this long passes while it is
 * pushed more than half way. */
#define AERON_XR_POINTER_WHEEL_US 150000u
/* The one-euro filter of the pointer's position, which is in screen widths.
 * A slow hand is filtered at the low cutoff, in Hz, so that a tremor doesn't
 * move the pointer. A fast hand raises the cutoff, so that the pointer
 * doesn't lag. */
#define AERON_XR_POINTER_MIN_CUTOFF 1.5f
#define AERON_XR_POINTER_SPEED_CUTOFF 40.0f
#define AERON_XR_POINTER_DERIVATIVE_CUTOFF 1.0f

static struct {
	/* The hand that points at the screen in this frame, or -1. */
	int hand;
	/* The hand that last pressed its trigger on the screen: it wins when
	 * both hands point at the screen. */
	int preferred;
	/* The real mouse moved. It keeps the pointer until a hand presses its
	 * trigger on the screen. */
	int mouse_has_pointer;
	/* The mouse buttons that this code set in the snapshot last frame. */
	uint32_t buttons;
	/* A hand's buttons that were already down when the hand started to
	 * point at the screen. They don't click until they are released. */
	uint32_t held_before[2];
	/* The filtered position on the screen, from 0 to 1, and its speed. */
	int      filtered;
	float    u, v;
	float    du, dv;
	uint64_t updated_us;
	/* The pointer stays at `hold_x`, `hold_y` until this time. */
	uint64_t hold_until_us;
	int      hold_x, hold_y;
	/* The last press of each button, for the double click. */
	uint64_t pressed_us[2];
	int      pressed_x[2], pressed_y[2];
	uint64_t wheel_us;
	/* The pointing hand that was last logged. */
	int logged_hand;
} g_pointer = { .hand = -1, .preferred = 1, .logged_hand = -1 };

int Aeron_XrPointerHand(void) { return g_pointer.hand; }

/* The panel of the last Aeron_XrPanelBegin. */
static struct {
	int              valid;
	AeronXrPanelDesc desc;
} g_panel;

void AeronXr_SetPanel(const struct AeronXrPanelDesc* desc) {
	g_panel.valid = desc != NULL;
	if (desc)
		g_panel.desc = *desc;
}

static float panel_height_m(void) {
	return g_panel.desc.width_m * (float)g_panel.desc.height_px / (float)g_panel.desc.width_px;
}

int AeronXr_PointerActive(void) { return g_pointer.hand >= 0; }

static int pointer_hit(const AeronHandState* hand, const AeronViewPose* screen, float width, float height,
					   float* u, float* v);

int Aeron_XrPanelHit(int hand, float* x, float* y) {
	float u, v;
	if (!g_panel.valid || hand < 0 || hand > 1 ||
		!pointer_hit(&g_hands.hands[hand], &g_panel.desc.pose, g_panel.desc.width_m, panel_height_m(), &u,
					 &v))
		return 0;
	if (x)
		*x = u * (float)g_panel.desc.width_px;
	if (y)
		*y = v * (float)g_panel.desc.height_px;
	return 1;
}

int Aeron_XrPanelPoint(float x, float y, float point_m[3]) {
	float rotation[9];
	if (!g_panel.valid)
		return 0;
	hand_pose_rotation(&g_panel.desc.pose, rotation);
	const float across = (x / (float)g_panel.desc.width_px - 0.5f) * g_panel.desc.width_m;
	const float up     = (0.5f - y / (float)g_panel.desc.height_px) * panel_height_m();
	for (int axis = 0; axis < 3; axis++)
		point_m[axis] = g_panel.desc.pose.position_m[axis] + rotation[axis * 3 + 0] * across +
						rotation[axis * 3 + 1] * up;
	return 1;
}

/* Finds where a hand's aim ray meets the virtual screen. `u` and `v` go from
 * 0 to 1 over the screen, from its top left corner. Returns 0 when the ray
 * misses the screen or comes from behind it. */
static int pointer_hit(const AeronHandState* hand, const AeronViewPose* screen, float width, float height,
					   float* u, float* v) {
	float rotation[9], direction[3], local[3];
	if (!hand->connected || !hand->tracked)
		return 0;
	hand_pose_rotation(screen, rotation);
	/* The screen's X, Y and Z axes are the columns of its rotation. Its Z
	 * axis points at the viewer. */
	const float normal[3] = { rotation[2], rotation[5], rotation[8] };
	hand_pose_forward(&hand->aim, direction);
	const float facing = direction[0] * normal[0] + direction[1] * normal[1] + direction[2] * normal[2];
	if (facing > -1e-4f)
		return 0;
	float distance = 0.0f;
	for (int i = 0; i < 3; i++)
		distance += (screen->position_m[i] - hand->aim.position_m[i]) * normal[i];
	distance /= facing;
	if (distance <= 0.0f)
		return 0;
	for (int i = 0; i < 3; i++)
		local[i] = hand->aim.position_m[i] + distance * direction[i] - screen->position_m[i];
	*u = 0.5f + (local[0] * rotation[0] + local[1] * rotation[3] + local[2] * rotation[6]) / width;
	*v = 0.5f - (local[0] * rotation[1] + local[1] * rotation[4] + local[2] * rotation[7]) / height;
	return *u >= 0.0f && *u <= 1.0f && *v >= 0.0f && *v <= 1.0f;
}

static float pointer_low_pass(float cutoff, float dt, float value, float previous) {
	const float alpha = 1.0f / (1.0f + 1.0f / (2.0f * 3.14159265f * cutoff * dt));
	return previous + alpha * (value - previous);
}

/* Smooths the pointer's position with a one-euro filter. */
static void pointer_filter(float u, float v, uint64_t now) {
	const float dt = (float)(now - g_pointer.updated_us) * 1e-6f;
	if (!g_pointer.filtered || dt <= 0.0f || dt > 0.25f) {
		g_pointer.u  = u;
		g_pointer.v  = v;
		g_pointer.du = 0.0f;
		g_pointer.dv = 0.0f;
	} else {
		g_pointer.du =
			pointer_low_pass(AERON_XR_POINTER_DERIVATIVE_CUTOFF, dt, (u - g_pointer.u) / dt, g_pointer.du);
		g_pointer.dv =
			pointer_low_pass(AERON_XR_POINTER_DERIVATIVE_CUTOFF, dt, (v - g_pointer.v) / dt, g_pointer.dv);
		const float speed  = sqrtf(g_pointer.du * g_pointer.du + g_pointer.dv * g_pointer.dv);
		const float cutoff = AERON_XR_POINTER_MIN_CUTOFF + AERON_XR_POINTER_SPEED_CUTOFF * speed;
		g_pointer.u        = pointer_low_pass(cutoff, dt, u, g_pointer.u);
		g_pointer.v        = pointer_low_pass(cutoff, dt, v, g_pointer.v);
	}
	g_pointer.filtered   = 1;
	g_pointer.updated_us = now;
}

/* The mouse buttons that a hand holds: the trigger is the left button and
 * the squeeze is the right one. */
static uint32_t pointer_hand_buttons(const AeronHandState* hand) {
	return ((hand->buttons & (1u << AERON_VR_HAND_BUTTON_TRIGGER)) ? AERON_MOUSE_BUTTON_LEFT : 0) |
		   ((hand->buttons & (1u << AERON_VR_HAND_BUTTON_SQUEEZE)) ? AERON_MOUSE_BUTTON_RIGHT : 0);
}

void AeronXr_UpdatePointer(AeronInputSnapshot* input) {
	AeronViewPose  screen;
	float          width = 0.0f, height = 0.0f;
	float          u[2] = { 0.0f, 0.0f }, v[2] = { 0.0f, 0.0f };
	int            hit[2]  = { 0, 0 };
	uint32_t       held[2] = { 0, 0 };
	const uint64_t now     = Aeron_NowUs();
	const uint32_t was     = g_pointer.buttons;
	const int      usable  = !g_aeron.relative_mouse_enabled && g_aeron.logical_width > 0 &&
							 g_aeron.logical_height > 0 && Aeron_XrScreenQuad(&screen, &width, &height);

	/* Take back the buttons that this code set last frame: the snapshot keeps
	 * its buttons from one frame to the next. */
	input->mouse.buttons &= ~was;
	g_pointer.buttons = 0;
	g_pointer.hand    = -1;

	for (int hand = 0; hand < 2; hand++) {
		const AeronHandState* state = &g_hands.hands[hand];
		held[hand]                  = pointer_hand_buttons(state);
		hit[hand] = usable && pointer_hit(state, &screen, width, height, &u[hand], &v[hand]);
		/* A button that is already down when the hand reaches the screen
		 * doesn't click. */
		if (!hit[hand])
			g_pointer.held_before[hand] = held[hand];
		else
			g_pointer.held_before[hand] &= held[hand];
		if (hit[hand] && (held[hand] & ~g_pointer.held_before[hand] & AERON_MOUSE_BUTTON_LEFT)) {
			g_pointer.preferred         = hand;
			g_pointer.mouse_has_pointer = 0;
		}
	}
	/* The real mouse moved or clicked: it has the pointer again. */
	if (input->mouse.relative_x != 0.0f || input->mouse.relative_y != 0.0f || input->mouse.pressed_buttons)
		g_pointer.mouse_has_pointer = 1;

	int hand = -1;
	if (!g_pointer.mouse_has_pointer) {
		if (hit[g_pointer.preferred])
			hand = g_pointer.preferred;
		else if (hit[1 - g_pointer.preferred])
			hand = 1 - g_pointer.preferred;
	}
	if (g_hands.log_inputs && hand != g_pointer.logged_hand) {
		if (hand < 0)
			Aeron_LogInfo("aeron.xr", "pointer: the mouse");
		else
			Aeron_LogInfo("aeron.xr", "pointer: the %s hand, at %.2f, %.2f of the screen", g_handNames[hand],
						  u[hand], v[hand]);
		g_pointer.logged_hand = hand;
	}
	if (hand < 0) {
		g_pointer.filtered = 0;
		/* The buttons that the hand held are released. */
		input->mouse.released_buttons |= was & ~input->mouse.buttons;
		return;
	}
	g_pointer.hand = hand;

	pointer_filter(u[hand], v[hand], now);
	int x = (int)(g_pointer.u * (float)g_aeron.logical_width);
	int y = (int)(g_pointer.v * (float)g_aeron.logical_height);
	x     = SDL_clamp(x, 0, g_aeron.logical_width - 1);
	y     = SDL_clamp(y, 0, g_aeron.logical_height - 1);

	const uint32_t buttons  = held[hand] & ~g_pointer.held_before[hand];
	const uint32_t pressed  = buttons & ~was;
	const uint32_t released = was & ~buttons;
	if (pressed) {
		g_pointer.hold_until_us = now + AERON_XR_POINTER_PRESS_HOLD_US;
		g_pointer.hold_x        = x;
		g_pointer.hold_y        = y;
	}
	if (now < g_pointer.hold_until_us) {
		x = g_pointer.hold_x;
		y = g_pointer.hold_y;
	}

	input->mouse.x              = x;
	input->mouse.y              = y;
	input->mouse.inside_content = 1;
	input->mouse.buttons |= buttons;
	input->mouse.pressed_buttons |= pressed;
	input->mouse.released_buttons |= released & ~input->mouse.buttons;
	g_pointer.buttons = buttons;

	/* A double click: the same button, pressed twice at about the same
	 * place. The distance allowed follows the size of the picture. */
	const int reach = SDL_max(8, g_aeron.logical_height / 60);
	for (int button = 0; button < 2; button++) {
		const uint32_t mask = button == 0 ? AERON_MOUSE_BUTTON_LEFT : AERON_MOUSE_BUTTON_RIGHT;
		if (!(pressed & mask))
			continue;
		if (g_pointer.pressed_us[button] &&
			now - g_pointer.pressed_us[button] < AERON_XR_POINTER_DOUBLE_CLICK_US &&
			abs(x - g_pointer.pressed_x[button]) <= reach && abs(y - g_pointer.pressed_y[button]) <= reach) {
			input->mouse.double_clicked_buttons |= mask;
			g_pointer.pressed_us[button] = 0;
		} else {
			g_pointer.pressed_us[button] = now;
			g_pointer.pressed_x[button]  = x;
			g_pointer.pressed_y[button]  = y;
		}
	}

	const float stick = g_hands.hands[hand].axes[AERON_VR_HAND_AXIS_THUMBSTICK_Y];
	if (fabsf(stick) <= 0.5f) {
		g_pointer.wheel_us = 0;
	} else if (now >= g_pointer.wheel_us) {
		input->mouse.wheel_y += stick > 0.0f ? 1 : -1;
		g_pointer.wheel_us = now + AERON_XR_POINTER_WHEEL_US;
	}
}

int AeronXr_HandHasRumble(int hand) { return hand >= 0 && hand < 2 && g_hands.hands[hand].connected; }

int AeronXr_RumbleHand(int hand, uint16_t low_frequency_rumble, uint16_t high_frequency_rumble,
					   uint32_t duration_ms) {
	if (hand < 0 || hand >= 2)
		return 0;
	AeronHandHaptic* haptic = &g_hands.haptics[hand];
	const uint16_t   strength =
		low_frequency_rumble > high_frequency_rumble ? low_frequency_rumble : high_frequency_rumble;
	const uint64_t now = Aeron_NowUs();
	if (strength == 0 || duration_ms == 0) {
		if (haptic->amplitude > 0.0f) {
			haptic->amplitude = 0.0f;
#if AERON_XR
			xr_apply_haptic(hand, 0.0f, 0);
#endif
		}
		return g_hands.hands[hand].connected;
	}
	if (!g_hands.hands[hand].connected)
		return 0;
	haptic->amplitude = (float)strength / 65535.0f;
	/* The game's force feedback uses the largest duration for an endless
	 * effect. */
	haptic->end_us   = duration_ms == UINT32_MAX ? UINT64_MAX : now + (uint64_t)duration_ms * 1000u;
	haptic->renew_us = 0;
	hand_update_haptic(hand, now);
	return 1;
}

#if AERON_XR

typedef struct AeronXrActionDesc {
	const char*  name;
	const char*  label;
	XrActionType type;
} AeronXrActionDesc;

static const AeronXrActionDesc g_xrActions[AERON_XR_ACTION_COUNT] = {
	[AERON_XR_ACTION_TRIGGER]          = { "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT },
	[AERON_XR_ACTION_SQUEEZE]          = { "squeeze", "Squeeze", XR_ACTION_TYPE_FLOAT_INPUT },
	[AERON_XR_ACTION_THUMBSTICK]       = { "thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT },
	[AERON_XR_ACTION_TRACKPAD]         = { "trackpad", "Trackpad", XR_ACTION_TYPE_VECTOR2F_INPUT },
	[AERON_XR_ACTION_TRIGGER_CLICK]    = { "trigger_click", "Trigger Click", XR_ACTION_TYPE_BOOLEAN_INPUT },
	[AERON_XR_ACTION_SQUEEZE_CLICK]    = { "squeeze_click", "Squeeze Click", XR_ACTION_TYPE_BOOLEAN_INPUT },
	[AERON_XR_ACTION_PRIMARY]          = { "primary", "Primary Button", XR_ACTION_TYPE_BOOLEAN_INPUT },
	[AERON_XR_ACTION_SECONDARY]        = { "secondary", "Secondary Button", XR_ACTION_TYPE_BOOLEAN_INPUT },
	[AERON_XR_ACTION_THUMBSTICK_CLICK] = { "thumbstick_click", "Thumbstick Click",
										   XR_ACTION_TYPE_BOOLEAN_INPUT },
	[AERON_XR_ACTION_TRACKPAD_CLICK]   = { "trackpad_click", "Trackpad Click", XR_ACTION_TYPE_BOOLEAN_INPUT },
	[AERON_XR_ACTION_MENU]             = { "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT },
	[AERON_XR_ACTION_GRIP_POSE]        = { "grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT },
	[AERON_XR_ACTION_AIM_POSE]         = { "aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT },
	[AERON_XR_ACTION_HAPTIC]           = { "haptic", "Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT },
};

/* One suggested binding: the action, the hands it is for, and the input's
 * path below the hand's path. */
#define XR_LEFT 1
#define XR_RIGHT 2
#define XR_BOTH 3

typedef struct AeronXrBinding {
	uint8_t     action;
	uint8_t     hands;
	const char* path;
} AeronXrBinding;

/* Every controller has the two poses and the vibration. */
#define XR_COMMON_BINDINGS                                                                                   \
	{ AERON_XR_ACTION_GRIP_POSE, XR_BOTH, "/input/grip/pose" },                                              \
		{ AERON_XR_ACTION_AIM_POSE, XR_BOTH, "/input/aim/pose" },                                            \
		{ AERON_XR_ACTION_HAPTIC, XR_BOTH, "/output/haptic" }

/* The inputs of the Touch controllers. The Touch Pro, the Touch Plus and the
 * HP controller have them under the same paths, except that the HP
 * controller has a menu button on both hands. */
#define XR_TOUCH_BINDINGS                                                                                    \
	{ AERON_XR_ACTION_TRIGGER, XR_BOTH, "/input/trigger/value" },                                            \
		{ AERON_XR_ACTION_SQUEEZE, XR_BOTH, "/input/squeeze/value" },                                        \
		{ AERON_XR_ACTION_THUMBSTICK, XR_BOTH, "/input/thumbstick" },                                        \
		{ AERON_XR_ACTION_THUMBSTICK_CLICK, XR_BOTH, "/input/thumbstick/click" },                            \
		{ AERON_XR_ACTION_PRIMARY, XR_LEFT, "/input/x/click" },                                              \
		{ AERON_XR_ACTION_SECONDARY, XR_LEFT, "/input/y/click" },                                            \
		{ AERON_XR_ACTION_PRIMARY, XR_RIGHT, "/input/a/click" },                                             \
		{ AERON_XR_ACTION_SECONDARY, XR_RIGHT, "/input/b/click" }

static const AeronXrBinding g_xrSimpleBindings[] = {
	{ AERON_XR_ACTION_TRIGGER, XR_BOTH, "/input/select/click" },
	{ AERON_XR_ACTION_TRIGGER_CLICK, XR_BOTH, "/input/select/click" },
	{ AERON_XR_ACTION_MENU, XR_BOTH, "/input/menu/click" },
	XR_COMMON_BINDINGS,
};

static const AeronXrBinding g_xrTouchBindings[] = {
	XR_TOUCH_BINDINGS,
	{ AERON_XR_ACTION_MENU, XR_LEFT, "/input/menu/click" },
	XR_COMMON_BINDINGS,
};

static const AeronXrBinding g_xrHpBindings[] = {
	XR_TOUCH_BINDINGS,
	{ AERON_XR_ACTION_MENU, XR_BOTH, "/input/menu/click" },
	XR_COMMON_BINDINGS,
};

/* The Index controller has no menu button for applications. Its squeeze and
 * its trackpad have no click: the squeeze's is derived from its value, and
 * the trackpad's from its force. */
static const AeronXrBinding g_xrIndexBindings[] = {
	{ AERON_XR_ACTION_TRIGGER, XR_BOTH, "/input/trigger/value" },
	{ AERON_XR_ACTION_TRIGGER_CLICK, XR_BOTH, "/input/trigger/click" },
	{ AERON_XR_ACTION_SQUEEZE, XR_BOTH, "/input/squeeze/value" },
	{ AERON_XR_ACTION_THUMBSTICK, XR_BOTH, "/input/thumbstick" },
	{ AERON_XR_ACTION_THUMBSTICK_CLICK, XR_BOTH, "/input/thumbstick/click" },
	{ AERON_XR_ACTION_TRACKPAD, XR_BOTH, "/input/trackpad" },
	{ AERON_XR_ACTION_TRACKPAD_CLICK, XR_BOTH, "/input/trackpad/force" },
	{ AERON_XR_ACTION_PRIMARY, XR_BOTH, "/input/a/click" },
	{ AERON_XR_ACTION_SECONDARY, XR_BOTH, "/input/b/click" },
	XR_COMMON_BINDINGS,
};

static const AeronXrBinding g_xrViveBindings[] = {
	{ AERON_XR_ACTION_TRIGGER, XR_BOTH, "/input/trigger/value" },
	{ AERON_XR_ACTION_TRIGGER_CLICK, XR_BOTH, "/input/trigger/click" },
	{ AERON_XR_ACTION_SQUEEZE, XR_BOTH, "/input/squeeze/click" },
	{ AERON_XR_ACTION_SQUEEZE_CLICK, XR_BOTH, "/input/squeeze/click" },
	{ AERON_XR_ACTION_TRACKPAD, XR_BOTH, "/input/trackpad" },
	{ AERON_XR_ACTION_TRACKPAD_CLICK, XR_BOTH, "/input/trackpad/click" },
	{ AERON_XR_ACTION_MENU, XR_BOTH, "/input/menu/click" },
	XR_COMMON_BINDINGS,
};

static const AeronXrBinding g_xrWmrBindings[] = {
	{ AERON_XR_ACTION_TRIGGER, XR_BOTH, "/input/trigger/value" },
	{ AERON_XR_ACTION_SQUEEZE, XR_BOTH, "/input/squeeze/click" },
	{ AERON_XR_ACTION_SQUEEZE_CLICK, XR_BOTH, "/input/squeeze/click" },
	{ AERON_XR_ACTION_THUMBSTICK, XR_BOTH, "/input/thumbstick" },
	{ AERON_XR_ACTION_THUMBSTICK_CLICK, XR_BOTH, "/input/thumbstick/click" },
	{ AERON_XR_ACTION_TRACKPAD, XR_BOTH, "/input/trackpad" },
	{ AERON_XR_ACTION_TRACKPAD_CLICK, XR_BOTH, "/input/trackpad/click" },
	{ AERON_XR_ACTION_MENU, XR_BOTH, "/input/menu/click" },
	XR_COMMON_BINDINGS,
};

/* A kind of controller that aeron suggests bindings for. `extension` is the
 * extension that the profile needs, or AERON_XR_EXT_COUNT when it is in the
 * core of OpenXR. */
typedef struct AeronXrProfile {
	const char*           path;
	AeronXrExtension      extension;
	const AeronXrBinding* bindings;
	size_t                binding_count;
} AeronXrProfile;

#define XR_PROFILE(path, extension, bindings) { path, extension, bindings, SDL_arraysize(bindings) }
static const AeronXrProfile g_xrProfiles[] = {
	XR_PROFILE("/interaction_profiles/khr/simple_controller", AERON_XR_EXT_COUNT, g_xrSimpleBindings),
	XR_PROFILE("/interaction_profiles/oculus/touch_controller", AERON_XR_EXT_COUNT, g_xrTouchBindings),
	XR_PROFILE("/interaction_profiles/valve/index_controller", AERON_XR_EXT_COUNT, g_xrIndexBindings),
	XR_PROFILE("/interaction_profiles/htc/vive_controller", AERON_XR_EXT_COUNT, g_xrViveBindings),
	XR_PROFILE("/interaction_profiles/microsoft/motion_controller", AERON_XR_EXT_COUNT, g_xrWmrBindings),
	XR_PROFILE("/interaction_profiles/hp/mixed_reality_controller", AERON_XR_EXT_HP_CONTROLLER,
			   g_xrHpBindings),
	XR_PROFILE("/interaction_profiles/facebook/touch_controller_pro", AERON_XR_EXT_TOUCH_PRO,
			   g_xrTouchBindings),
	XR_PROFILE("/interaction_profiles/meta/touch_controller_plus", AERON_XR_EXT_TOUCH_PLUS,
			   g_xrTouchBindings),
};

/* The largest binding table has 13 entries, some of them for both hands. */
#define XR_MAX_SUGGESTED_BINDINGS 32

static void xr_input_warn(const char* what, XrResult result) {
	char error[256];
	AeronXr_ResultError(error, sizeof error, what, result);
	Aeron_LogWarn("aeron.xr", "%s", error);
}

static int xr_input_fail(const char* what, XrResult result) {
	char error[256];
	AeronXr_ResultError(error, sizeof error, what, result);
	Aeron_LogWarn("aeron.xr", "%s; VR controllers are disabled", error);
	return 0;
}

/* Suggests the bindings of one kind of controller. A runtime may refuse a
 * profile it doesn't know: the others still apply. */
static void xr_suggest_profile(const AeronXrProfile* profile) {
	static const char* const hand_paths[2] = { "/user/hand/left", "/user/hand/right" };
	XrActionSuggestedBinding bindings[XR_MAX_SUGGESTED_BINDINGS];
	uint32_t                 count = 0;
	XrResult                 result;

	if (profile->extension != AERON_XR_EXT_COUNT && !g_xr.extension_enabled[profile->extension])
		return;
	for (size_t i = 0; i < profile->binding_count; i++) {
		const AeronXrBinding* binding = &profile->bindings[i];
		for (int hand = 0; hand < 2; hand++) {
			char path[128];
			if (!(binding->hands & (1 << hand)) || count >= XR_MAX_SUGGESTED_BINDINGS)
				continue;
			SDL_snprintf(path, sizeof path, "%s%s", hand_paths[hand], binding->path);
			bindings[count].action = g_xr.input.actions[binding->action];
			result                 = g_xr.fn.xrStringToPath(g_xr.instance, path, &bindings[count].binding);
			if (XR_FAILED(result)) {
				Aeron_LogWarn("aeron.xr", "the OpenXR runtime refused the path %s", path);
				continue;
			}
			count++;
		}
	}
	XrInteractionProfileSuggestedBinding suggested = {
		.type                   = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
		.countSuggestedBindings = count,
		.suggestedBindings      = bindings,
	};
	result = g_xr.fn.xrStringToPath(g_xr.instance, profile->path, &suggested.interactionProfile);
	if (XR_SUCCEEDED(result))
		result = g_xr.fn.xrSuggestInteractionProfileBindings(g_xr.instance, &suggested);
	if (XR_FAILED(result)) {
		char error[256];
		AeronXr_ResultError(error, sizeof error, profile->path, result);
		Aeron_LogWarn("aeron.xr", "the OpenXR runtime refused the bindings of %s", error);
	}
}

static int xr_create_actions(void) {
	AeronXrInputState* input = &g_xr.input;
	XrResult           result;

	if (XR_FAILED(result = g_xr.fn.xrStringToPath(g_xr.instance, "/user/hand/left", &input->hand_paths[0])) ||
		XR_FAILED(result = g_xr.fn.xrStringToPath(g_xr.instance, "/user/hand/right", &input->hand_paths[1])))
		return xr_input_fail("could not create the paths of the hands", result);

	XrActionSetCreateInfo set_info = { .type = XR_TYPE_ACTION_SET_CREATE_INFO };
	SDL_strlcpy(set_info.actionSetName, "aeron", sizeof set_info.actionSetName);
	SDL_strlcpy(set_info.localizedActionSetName, "Controllers", sizeof set_info.localizedActionSetName);
	result = g_xr.fn.xrCreateActionSet(g_xr.instance, &set_info, &input->action_set);
	if (XR_FAILED(result)) {
		input->action_set = XR_NULL_HANDLE;
		return xr_input_fail("could not create the action set", result);
	}
	for (int i = 0; i < AERON_XR_ACTION_COUNT; i++) {
		XrActionCreateInfo info = {
			.type                = XR_TYPE_ACTION_CREATE_INFO,
			.actionType          = g_xrActions[i].type,
			.countSubactionPaths = 2,
			.subactionPaths      = input->hand_paths,
		};
		SDL_strlcpy(info.actionName, g_xrActions[i].name, sizeof info.actionName);
		SDL_strlcpy(info.localizedActionName, g_xrActions[i].label, sizeof info.localizedActionName);
		result = g_xr.fn.xrCreateAction(input->action_set, &info, &input->actions[i]);
		if (XR_FAILED(result))
			return xr_input_fail("could not create an action", result);
	}
	for (size_t i = 0; i < SDL_arraysize(g_xrProfiles); i++)
		xr_suggest_profile(&g_xrProfiles[i]);

	const XrSessionActionSetsAttachInfo attach = {
		.type            = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
		.countActionSets = 1,
		.actionSets      = &input->action_set,
	};
	result = g_xr.fn.xrAttachSessionActionSets(g_xr.machine.session, &attach);
	if (XR_FAILED(result))
		return xr_input_fail("could not attach the actions to the session", result);

	for (int hand = 0; hand < 2; hand++) {
		XrActionSpaceCreateInfo info = {
			.type              = XR_TYPE_ACTION_SPACE_CREATE_INFO,
			.action            = input->actions[AERON_XR_ACTION_GRIP_POSE],
			.subactionPath     = input->hand_paths[hand],
			.poseInActionSpace = { .orientation = { 0.0f, 0.0f, 0.0f, 1.0f } },
		};
		result = g_xr.fn.xrCreateActionSpace(g_xr.machine.session, &info, &input->grip_spaces[hand]);
		if (XR_SUCCEEDED(result)) {
			info.action = input->actions[AERON_XR_ACTION_AIM_POSE];
			result      = g_xr.fn.xrCreateActionSpace(g_xr.machine.session, &info, &input->aim_spaces[hand]);
		}
		if (XR_FAILED(result))
			return xr_input_fail("could not create the space of a hand", result);
	}
	return 1;
}

void AeronXrInput_Create(void) {
	SDL_zero(g_xr.input);
	if (!xr_create_actions()) {
		AeronXrInput_Destroy();
		return;
	}
	g_xr.input.ready = 1;
}

void AeronXrInput_Destroy(void) {
	AeronXrInputState* input = &g_xr.input;
	for (int hand = 0; hand < 2; hand++) {
		if (input->grip_spaces[hand])
			g_xr.fn.xrDestroySpace(input->grip_spaces[hand]);
		if (input->aim_spaces[hand])
			g_xr.fn.xrDestroySpace(input->aim_spaces[hand]);
	}
	/* Destroying the set destroys its actions. */
	if (input->action_set)
		g_xr.fn.xrDestroyActionSet(input->action_set);
	SDL_zero(*input);
}

/* Logs which kind of controller a hand holds, when it changed since the
 * last log line. */
static void xr_log_profile(int hand) {
	XrInteractionProfileState state                    = { .type = XR_TYPE_INTERACTION_PROFILE_STATE };
	char                      name[XR_MAX_PATH_LENGTH] = "none";
	uint32_t                  length                   = 0;
	const XrResult            result =
		g_xr.fn.xrGetCurrentInteractionProfile(g_xr.machine.session, g_xr.input.hand_paths[hand], &state);
	if (XR_FAILED(result)) {
		xr_input_warn("could not read the interaction profile of a hand", result);
		return;
	}
	if (g_xr.input.profile_logged[hand] && state.interactionProfile == g_xr.input.logged_profiles[hand])
		return;
	g_xr.input.profile_logged[hand]  = 1;
	g_xr.input.logged_profiles[hand] = state.interactionProfile;
	if (state.interactionProfile != XR_NULL_PATH &&
		XR_FAILED(
			g_xr.fn.xrPathToString(g_xr.instance, state.interactionProfile, sizeof name, &length, name)))
		SDL_strlcpy(name, "unknown", sizeof name);
	Aeron_LogInfo("aeron.xr", "%s hand: interaction profile %s", g_handNames[hand], name);
	/* A different controller has a different model. */
	AeronXrModels_Changed();
}

void AeronXrInput_ProfileChanged(void) {
	if (!g_xr.input.ready)
		return;
	for (int hand = 0; hand < 2; hand++)
		xr_log_profile(hand);
}

static XrActionStateGetInfo xr_state_info(AeronXrAction action, int hand) {
	const XrActionStateGetInfo info = {
		.type          = XR_TYPE_ACTION_STATE_GET_INFO,
		.action        = g_xr.input.actions[action],
		.subactionPath = g_xr.input.hand_paths[hand],
	};
	return info;
}

/* Each of these reads one action for one hand. They return 0 and set the
 * value to zero when the controller doesn't have the input. */
static int xr_get_boolean(AeronXrAction action, int hand, int* value) {
	const XrActionStateGetInfo info  = xr_state_info(action, hand);
	XrActionStateBoolean       state = { .type = XR_TYPE_ACTION_STATE_BOOLEAN };
	*value                           = 0;
	if (XR_FAILED(g_xr.fn.xrGetActionStateBoolean(g_xr.machine.session, &info, &state)) || !state.isActive)
		return 0;
	*value = state.currentState == XR_TRUE;
	return 1;
}

static int xr_get_float(AeronXrAction action, int hand, float* value) {
	const XrActionStateGetInfo info  = xr_state_info(action, hand);
	XrActionStateFloat         state = { .type = XR_TYPE_ACTION_STATE_FLOAT };
	*value                           = 0.0f;
	if (XR_FAILED(g_xr.fn.xrGetActionStateFloat(g_xr.machine.session, &info, &state)) || !state.isActive)
		return 0;
	*value = state.currentState;
	return 1;
}

static int xr_get_vector(AeronXrAction action, int hand, float* x, float* y) {
	const XrActionStateGetInfo info  = xr_state_info(action, hand);
	XrActionStateVector2f      state = { .type = XR_TYPE_ACTION_STATE_VECTOR2F };
	*x                               = 0.0f;
	*y                               = 0.0f;
	if (XR_FAILED(g_xr.fn.xrGetActionStateVector2f(g_xr.machine.session, &info, &state)) || !state.isActive)
		return 0;
	*x = state.currentState.x;
	*y = state.currentState.y;
	return 1;
}

/* Locates a hand's space in the rig's space, at the time the frame is
 * shown. Returns 0 when the runtime doesn't know the whole pose. */
static int xr_locate_hand(XrSpace space, AeronViewPose* out) {
	XrSpaceLocation            location = { .type = XR_TYPE_SPACE_LOCATION };
	const XrSpaceLocationFlags needed =
		XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if (XR_FAILED(g_xr.fn.xrLocateSpace(space, g_xr.rig_space, g_xr.frame_display_time, &location)) ||
		(location.locationFlags & needed) != needed)
		return 0;
	out->position_m[0]  = location.pose.position.x;
	out->position_m[1]  = location.pose.position.y;
	out->position_m[2]  = location.pose.position.z;
	out->orientation[0] = location.pose.orientation.x;
	out->orientation[1] = location.pose.orientation.y;
	out->orientation[2] = location.pose.orientation.z;
	out->orientation[3] = location.pose.orientation.w;
	return 1;
}

/* A button for an input that has a value: the controller's own click when
 * it has one, and the value against a threshold when it doesn't. */
static int xr_read_click(AeronXrAction click, int hand, float value, int was_down) {
	int down;
	if (xr_get_boolean(click, hand, &down))
		return down;
	return value >= (was_down ? AERON_VR_HAND_CLICK_UP : AERON_VR_HAND_CLICK_DOWN);
}

static void xr_read_hand(int hand, AeronHandState* out) {
	const uint32_t             was_buttons = g_hands.hands[hand].buttons;
	const XrActionStateGetInfo pose_info   = xr_state_info(AERON_XR_ACTION_GRIP_POSE, hand);
	XrActionStatePose          pose        = { .type = XR_TYPE_ACTION_STATE_POSE };
	int                        down;

	/* The grip pose is active while the controller is on. */
	if (XR_FAILED(g_xr.fn.xrGetActionStatePose(g_xr.machine.session, &pose_info, &pose)) || !pose.isActive)
		return;
	out->connected = 1;
	out->tracked   = g_xr.frame_open && xr_locate_hand(g_xr.input.grip_spaces[hand], &out->grip) &&
					 xr_locate_hand(g_xr.input.aim_spaces[hand], &out->aim);

	xr_get_float(AERON_XR_ACTION_TRIGGER, hand, &out->axes[AERON_VR_HAND_AXIS_TRIGGER]);
	xr_get_float(AERON_XR_ACTION_SQUEEZE, hand, &out->axes[AERON_VR_HAND_AXIS_SQUEEZE]);
	xr_get_vector(AERON_XR_ACTION_THUMBSTICK, hand, &out->axes[AERON_VR_HAND_AXIS_THUMBSTICK_X],
				  &out->axes[AERON_VR_HAND_AXIS_THUMBSTICK_Y]);
	xr_get_vector(AERON_XR_ACTION_TRACKPAD, hand, &out->axes[AERON_VR_HAND_AXIS_TRACKPAD_X],
				  &out->axes[AERON_VR_HAND_AXIS_TRACKPAD_Y]);

	if (xr_read_click(AERON_XR_ACTION_TRIGGER_CLICK, hand, out->axes[AERON_VR_HAND_AXIS_TRIGGER],
					  was_buttons & (1u << AERON_VR_HAND_BUTTON_TRIGGER)))
		out->buttons |= 1u << AERON_VR_HAND_BUTTON_TRIGGER;
	if (xr_read_click(AERON_XR_ACTION_SQUEEZE_CLICK, hand, out->axes[AERON_VR_HAND_AXIS_SQUEEZE],
					  was_buttons & (1u << AERON_VR_HAND_BUTTON_SQUEEZE)))
		out->buttons |= 1u << AERON_VR_HAND_BUTTON_SQUEEZE;

	static const struct {
		AeronXrAction     action;
		AeronVrHandButton button;
	} buttons[] = {
		{ AERON_XR_ACTION_PRIMARY, AERON_VR_HAND_BUTTON_PRIMARY },
		{ AERON_XR_ACTION_SECONDARY, AERON_VR_HAND_BUTTON_SECONDARY },
		{ AERON_XR_ACTION_THUMBSTICK_CLICK, AERON_VR_HAND_BUTTON_THUMBSTICK },
		{ AERON_XR_ACTION_TRACKPAD_CLICK, AERON_VR_HAND_BUTTON_TRACKPAD },
		{ AERON_XR_ACTION_MENU, AERON_VR_HAND_BUTTON_MENU },
	};

	for (size_t i = 0; i < SDL_arraysize(buttons); i++) {
		if (xr_get_boolean(buttons[i].action, hand, &down) && down)
			out->buttons |= 1u << buttons[i].button;
	}
}

/* Reads the two hands from the headset. The runtime only gives the actions'
 * states while the session has the user's input focus: without it, the
 * hands are off. */
static void xr_read_hands(AeronHandState hands[2]) {
	AeronXrInputState* input = &g_xr.input;

	input->synced = 0;
	if (!input->ready || g_xr.failed || !g_xr.machine.running || !g_xr.machine.focused)
		return;
	const XrActiveActionSet active = { .actionSet = input->action_set, .subactionPath = XR_NULL_PATH };
	const XrActionsSyncInfo info   = {
		.type                  = XR_TYPE_ACTIONS_SYNC_INFO,
		.countActiveActionSets = 1,
		.activeActionSets      = &active,
	};
	const XrResult result = g_xr.fn.xrSyncActions(g_xr.machine.session, &info);
	if (XR_FAILED(result)) {
		if (!input->sync_failed)
			xr_input_warn("could not read the VR controllers", result);
		input->sync_failed = 1;
		return;
	}
	input->sync_failed = 0;
	/* XR_SESSION_NOT_FOCUSED: the focus went away since the last event. */
	if (result != XR_SUCCESS)
		return;
	input->synced = 1;
	for (int hand = 0; hand < 2; hand++) {
		xr_read_hand(hand, &hands[hand]);
		/* Some runtimes send no event for the first profile of a hand. */
		if (hands[hand].connected && !g_hands.hands[hand].connected)
			xr_log_profile(hand);
	}
	AeronXrModels_Update();
}

static void xr_apply_haptic(int hand, float amplitude, uint64_t duration_us) {
	AeronXrInputState* input = &g_xr.input;
	if (!input->ready || g_xr.failed || !g_xr.machine.running)
		return;
	const XrHapticActionInfo info = {
		.type          = XR_TYPE_HAPTIC_ACTION_INFO,
		.action        = input->actions[AERON_XR_ACTION_HAPTIC],
		.subactionPath = input->hand_paths[hand],
	};
	if (amplitude <= 0.0f) {
		g_xr.fn.xrStopHapticFeedback(g_xr.machine.session, &info);
		return;
	}
	const XrHapticVibration vibration = {
		.type      = XR_TYPE_HAPTIC_VIBRATION,
		.duration  = (XrDuration)duration_us * 1000,
		.frequency = XR_FREQUENCY_UNSPECIFIED,
		.amplitude = amplitude > 1.0f ? 1.0f : amplitude,
	};
	g_xr.fn.xrApplyHapticFeedback(g_xr.machine.session, &info, (const XrHapticBaseHeader*)&vibration);
}

#endif /* AERON_XR */
