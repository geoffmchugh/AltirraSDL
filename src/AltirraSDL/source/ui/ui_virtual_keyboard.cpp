//	AltirraSDL - Virtual on-screen keyboard
//	Displays a touch-friendly paged Atari XL/XE keyboard.  The original
//	photographic keyboard made all 62 keys too small on phones; this layout
//	keeps the full Atari key set while splitting typing, punctuation, and
//	editing controls across three views.

#include <stdafx.h>
#include <SDL3/SDL.h>
#include <imgui.h>

#include <vector>

#include "ui_virtual_keyboard.h"
#include "display_backend.h"
#include "gl_helpers.h"
#include "simulator.h"
#include "gtia.h"
#include <at/ataudio/pokey.h>
#include "uikeyboard.h"
#include "keyboard_data.h"
#include "netplay/netplay_input.h"

#ifdef __ANDROID__
#include "android_platform.h"
#endif

extern SDL_Window *g_pWindow;
extern float g_menuBarHeight;
extern ATUIKeyboardOptions g_kbdOpts;
extern IDisplayBackend *ATUIGetDisplayBackend();

static void GetUIWindowSize(int& winW, int& winH) {
	ImGuiIO& io = ImGui::GetIO();
	winW = (int)(io.DisplaySize.x + 0.5f);
	winH = (int)(io.DisplaySize.y + 0.5f);
	if (winW <= 0 || winH <= 0)
		SDL_GetWindowSize(g_pWindow, &winW, &winH);
}

// Native text input mode — when active, the mobile phone's soft keyboard
// is shown and typed characters are translated to Atari scancodes.
static bool s_nativeTextInputActive = false;

// ---------------------------------------------------------------------------
// Texture state
// ---------------------------------------------------------------------------
static SDL_Texture *s_sdlTexture = nullptr;
static uint32_t s_glTexture = 0;
static int s_texW = 0;
static int s_texH = 0;
static bool s_textureInited = false;
static bool s_useGL = false;

// ---------------------------------------------------------------------------
// Keyboard interaction state
// ---------------------------------------------------------------------------
static int s_focusedKey = -1;     // gamepad cursor index (-1 = none, kOSKKeyCount = close btn)
static int s_pressedKey = -1;     // currently pressed key (mouse/touch)
static int s_hoverKey = -1;       // key under mouse cursor
static bool s_closeRequested = false; // set by close button press
static int s_gamepadNavAxisX = 0;
static int s_gamepadNavAxisY = 0;
static int s_gamepadNavDir = -1;

// Modifier sticky/held state (matches uionscreenkeyboard.cpp pattern)
static bool s_shiftHeld = false;
static bool s_shiftSticky = false;
static bool s_controlHeld = false;
static bool s_controlSticky = false;

// Track which console switches are held by the virtual keyboard
static bool s_consoleStartHeld = false;
static bool s_consoleSelectHeld = false;
static bool s_consoleOptionHeld = false;

// Mobile keyboard presentation state. Controller navigation is handled here
// rather than by ImGui: gamepad events are intercepted before ImGui receives
// them so that they cannot leak into the running emulation.
enum class MobileAction : uint8_t {
	Key, Shift, Control, Page, Placement, NativeText, Close
};
struct MobileKey {
	const char *label;
	const char *oskLabel;
	MobileAction action;
	uint8_t forcedModifiers;       // bit 0=Shift, bit 1=Control
};
struct MobileHit {
	ImVec2 min;
	ImVec2 max;
	MobileKey key;
};
static std::vector<MobileHit> s_mobileHits;
static int s_mobilePage = 0;     // 0=ABC, 1=symbols, 2=Atari/editing
static int s_mobileFocus = -1;
static int s_mobilePressedHit = -1;
static int s_mobilePressedOSK = -1;
static int s_mobileFirstConsole = 0;
static int *s_mobilePlacement = nullptr;
struct MobileTouchHold {
	SDL_FingerID finger;
	int oskIndex;
	int hitIndex;
	int page;
};
static std::vector<MobileTouchHold> s_mobileTouchHolds;

// Previous-frame insets for display rect computation
static float s_lastBottomInset = 0;
static float s_lastRightInset = 0;

// Touch finger tracking (for mobile)
static SDL_FingerID s_touchFinger = 0;
static bool s_touchActive = false;

// ---------------------------------------------------------------------------
// Texture management
// ---------------------------------------------------------------------------
static ImTextureID EnsureTexture() {
	if (s_textureInited) {
		if (s_useGL)
			return (ImTextureID)(intptr_t)s_glTexture;
		return (ImTextureID)s_sdlTexture;
	}
	s_textureInited = true;

	// Load BMP from baked data
	SDL_IOStream *io = SDL_IOFromConstMem(kKeyboardBMPData, (size_t)kKeyboardBMPSize);
	if (!io)
		return (ImTextureID)0;

	SDL_Surface *bmpSurf = SDL_LoadBMP_IO(io, true);
	if (!bmpSurf)
		return (ImTextureID)0;

	// Convert to RGBA32 for GPU upload
	SDL_Surface *rgba = SDL_ConvertSurface(bmpSurf, SDL_PIXELFORMAT_RGBA32);
	SDL_DestroySurface(bmpSurf);
	if (!rgba)
		return (ImTextureID)0;

	s_texW = rgba->w;
	s_texH = rgba->h;

	IDisplayBackend *backend = ATUIGetDisplayBackend();
	s_useGL = backend && backend->GetType() == DisplayBackendType::OpenGL;

	if (s_useGL) {
		s_glTexture = GLCreateTexture2D(
			rgba->w, rgba->h, GL_RGBA8, GL_RGBA,
			GL_UNSIGNED_BYTE, rgba->pixels, true);
	} else {
		SDL_Renderer *renderer = SDL_GetRenderer(g_pWindow);
		if (renderer)
			s_sdlTexture = SDL_CreateTextureFromSurface(renderer, rgba);
	}

	SDL_DestroySurface(rgba);

	if (s_useGL)
		return (ImTextureID)(intptr_t)s_glTexture;
	return (ImTextureID)s_sdlTexture;
}

void ATUIVirtualKeyboard_Shutdown() {
	if (s_sdlTexture) {
		SDL_DestroyTexture(s_sdlTexture);
		s_sdlTexture = nullptr;
	}
	if (s_glTexture) {
		glDeleteTextures(1, &s_glTexture);
		s_glTexture = 0;
	}
	s_textureInited = false;
	s_texW = 0;
	s_texH = 0;
}

// ---------------------------------------------------------------------------
// Console switch helpers
// ---------------------------------------------------------------------------
static uint8_t GetConsoleBit(uint8_t scanCode) {
	switch (scanCode) {
		case 0x48: return 0x01;  // START
		case 0x49: return 0x02;  // SELECT
		case 0x4A: return 0x04;  // OPTION
		default:   return 0;
	}
}

static bool* GetConsoleHeldFlag(uint8_t scanCode) {
	switch (scanCode) {
		case 0x48: return &s_consoleStartHeld;
		case 0x49: return &s_consoleSelectHeld;
		case 0x4A: return &s_consoleOptionHeld;
		default:   return nullptr;
	}
}

// ---------------------------------------------------------------------------
// Modifier logic (replicates uionscreenkeyboard.cpp lines 355-495)
// ---------------------------------------------------------------------------
static bool IsShiftActive() {
	return s_shiftHeld || s_shiftSticky;
}

static bool IsControlActive() {
	return s_controlHeld || s_controlSticky;
}

// Apply the latched shift/ctrl state to the right destination: in
// netplay, the lockstep pipeline (so peers stay in sync); otherwise
// POKEY directly.  Mirrors the physical-keyboard gate at
// input_sdl3.cpp:693-703.
static void ApplyModifierState(ATPokeyEmulator &pokey) {
	const bool shift = s_shiftHeld || s_shiftSticky;
	const bool ctrl  = s_controlHeld || s_controlSticky;
	if (ATNetplayInput::IsActive()) {
		ATNetplayInput::OnLocalShiftCtrlState(shift, ctrl);
	} else {
		pokey.SetShiftKeyState(shift, !g_kbdOpts.mbFullRawKeys);
		pokey.SetControlKeyState(ctrl);
	}
}

static void HandleModifierPress(ATSimulator &sim, int index) {
	const ATOSKKeyDef &key = kOSKKeys[index];
	ATPokeyEmulator &pokey = sim.GetPokey();

	if (key.scanCode == 0x42) {
		// Shift — toggle sticky
		s_shiftSticky = !s_shiftSticky;
		s_shiftHeld = true;
	} else if (key.scanCode == 0x41) {
		// Control — toggle sticky
		s_controlSticky = !s_controlSticky;
		s_controlHeld = true;
	}
	ApplyModifierState(pokey);
}

static void HandleModifierRelease(ATSimulator &sim, int index) {
	const ATOSKKeyDef &key = kOSKKeys[index];
	ATPokeyEmulator &pokey = sim.GetPokey();

	if (key.scanCode == 0x42) {
		s_shiftHeld = false;
	} else if (key.scanCode == 0x41) {
		s_controlHeld = false;
	}
	ApplyModifierState(pokey);
}

static void ReleaseStickyModifiers(ATSimulator &sim) {
	ATPokeyEmulator &pokey = sim.GetPokey();

	const bool hadStickyModifier = s_shiftSticky || s_controlSticky;
	if (s_shiftSticky)   s_shiftSticky = false;
	if (s_controlSticky) s_controlSticky = false;
	if (hadStickyModifier)
		ApplyModifierState(pokey);
}

// ---------------------------------------------------------------------------
// Key press/release (replicates uionscreenkeyboard.cpp lines 355-459)
// ---------------------------------------------------------------------------
static void PressKey(ATSimulator &sim, int index) {
	if (index < 0 || index >= kOSKKeyCount)
		return;

	const ATOSKKeyDef &key = kOSKKeys[index];
	ATPokeyEmulator &pokey = sim.GetPokey();
	ATGTIAEmulator &gtia = sim.GetGTIA();

	if (key.flags & kOSKFlag_Console) {
		uint8_t bit = GetConsoleBit(key.scanCode);
		// Route through netplay so the peer sees the same console-switch
		// edge on the same lockstep frame; falls through to GTIA when
		// no session is live.
		ATNetplayInput::RouteConsoleSwitch(&gtia, bit, true);
		bool *flag = GetConsoleHeldFlag(key.scanCode);
		if (flag) *flag = true;
	} else if (key.flags & kOSKFlag_Break) {
		// Hardware Break has no lockstep encoding; silenced in-session
		// so peers don't diverge on a one-sided POKEY break event.
		if (!ATNetplayInput::ShouldSuppressBreak())
			pokey.PushBreak();
	} else if (key.flags & kOSKFlag_Reset) {
		// Same — WarmReset can't replicate over the wire.
		if (!ATNetplayInput::ShouldSuppressWarmReset())
			sim.WarmReset();
	} else if (key.flags & kOSKFlag_Toggle) {
		HandleModifierPress(sim, index);
	} else {
		uint8_t sc = key.scanCode;
		// Outside a netplay session, the on-screen keyboard folds
		// shift/ctrl into the scancode so PushRawKey sees a single
		// pre-shifted code (legacy behaviour from uionscreenkeyboard).
		// In-session we send the bare scancode + separate modifier state
		// — the apply side calls PushKey (cooked) which does its own
		// folding via SetShiftKeyState/SetControlKeyState.
		if (!ATNetplayInput::IsActive()) {
			if (pokey.GetShiftKeyState())
				sc += 0x40;
			if (pokey.GetControlKeyState())
				sc += 0x80;
		}
		ATNetplayInput::RouteRawKeyDown(&pokey, sc, !g_kbdOpts.mbFullRawKeys);
	}

	s_pressedKey = index;

#ifdef __ANDROID__
	ATAndroid_Vibrate(10);
#endif
}

static void ReleaseKey(ATSimulator &sim, int index) {
	if (index < 0 || index >= kOSKKeyCount)
		return;

	const ATOSKKeyDef &key = kOSKKeys[index];
	ATPokeyEmulator &pokey = sim.GetPokey();
	ATGTIAEmulator &gtia = sim.GetGTIA();

	if (key.flags & kOSKFlag_Console) {
		uint8_t bit = GetConsoleBit(key.scanCode);
		ATNetplayInput::RouteConsoleSwitch(&gtia, bit, false);
		bool *flag = GetConsoleHeldFlag(key.scanCode);
		if (flag) *flag = false;
	} else if (key.flags & kOSKFlag_Break) {
		// No release action
	} else if (key.flags & kOSKFlag_Reset) {
		// No release action
	} else if (key.flags & kOSKFlag_Toggle) {
		HandleModifierRelease(sim, index);
	} else {
		pokey.ReleaseRawKey(key.scanCode, !g_kbdOpts.mbFullRawKeys);
		// Auto-release sticky modifiers after a normal key press
		ReleaseStickyModifiers(sim);
	}

	if (s_pressedKey == index)
		s_pressedKey = -1;
}

void ATUIVirtualKeyboard_ReleaseAll(ATSimulator &sim) {
	for (const MobileTouchHold& hold : s_mobileTouchHolds) {
		if (hold.oskIndex >= 0)
			ReleaseKey(sim, hold.oskIndex);
	}
	if (!s_mobileTouchHolds.empty()) {
		s_mobileTouchHolds.clear();
		s_pressedKey = -1;
	}
	if (s_pressedKey >= 0) {
		ReleaseKey(sim, s_pressedKey);
		s_pressedKey = -1;
	}

	ATPokeyEmulator &pokey = sim.GetPokey();
	ATGTIAEmulator &gtia = sim.GetGTIA();

	// Clear both modifier axes through the same netplay-aware route used to
	// acquire them. Directly clearing POKEY here left the replicated modifier
	// state stuck on peers when focus was lost or the keyboard was closed.
	const bool hadModifier = s_shiftHeld || s_shiftSticky
		|| s_controlHeld || s_controlSticky;
	s_shiftHeld = false;
	s_shiftSticky = false;
	s_controlHeld = false;
	s_controlSticky = false;
	if (hadModifier)
		ApplyModifierState(pokey);
	if (s_consoleStartHeld)  { ATNetplayInput::RouteConsoleSwitch(&gtia, 0x01, false); s_consoleStartHeld = false; }
	if (s_consoleSelectHeld) { ATNetplayInput::RouteConsoleSwitch(&gtia, 0x02, false); s_consoleSelectHeld = false; }
	if (s_consoleOptionHeld) { ATNetplayInput::RouteConsoleSwitch(&gtia, 0x04, false); s_consoleOptionHeld = false; }

	// Turn off native text input mode when keyboard is dismissed.
	// SDL_StopTextInput hides the Android soft keyboard.  On desktop
	// we must re-enable text input so the cooked character path
	// (SDL_EVENT_TEXT_INPUT for non-US layouts, dead keys, IME) keeps
	// working for the physical keyboard.
	if (s_nativeTextInputActive) {
		s_nativeTextInputActive = false;
#ifdef __ANDROID__
		SDL_StopTextInput(g_pWindow);
#endif
	}

	s_touchActive = false;
	s_focusedKey = -1;
	s_gamepadNavAxisX = 0;
	s_gamepadNavAxisY = 0;
	s_gamepadNavDir = -1;
	s_mobilePressedHit = -1;
	s_mobilePressedOSK = -1;
}

// ---------------------------------------------------------------------------
// Key bounding box — computed once from kOSKKeys.  Used to crop the
// photographic border out of the keyboard BMP so the usable key area
// fills the panel edge-to-edge on Android.
// ---------------------------------------------------------------------------
static bool s_keyBoundsInited = false;
static float s_keyU0 = 0.0f, s_keyV0 = 0.0f;
static float s_keyU1 = 1.0f, s_keyV1 = 1.0f;
static float s_croppedAspect = 1024.0f / 448.0f;

static void EnsureKeyBounds() {
	if (s_keyBoundsInited)
		return;
	s_keyBoundsInited = true;

	float u0 = 1.0f, v0 = 1.0f, u1 = 0.0f, v1 = 0.0f;
	for (int i = 0; i < kOSKKeyCount; i++) {
		const ATOSKKeyDef &k = kOSKKeys[i];
		if (k.u0 < u0) u0 = k.u0;
		if (k.v0 < v0) v0 = k.v0;
		if (k.u1 > u1) u1 = k.u1;
		if (k.v1 > v1) v1 = k.v1;
	}

	// Small safety margin so the outermost key borders aren't clipped.
	const float kMargin = 0.003f;
	u0 -= kMargin; if (u0 < 0.0f) u0 = 0.0f;
	v0 -= kMargin; if (v0 < 0.0f) v0 = 0.0f;
	u1 += kMargin; if (u1 > 1.0f) u1 = 1.0f;
	v1 += kMargin; if (v1 > 1.0f) v1 = 1.0f;

	s_keyU0 = u0;
	s_keyV0 = v0;
	s_keyU1 = u1;
	s_keyV1 = v1;

	// Cropped aspect ratio, computed against the reference texture
	// size.  The generated BMP (tools/bake_keyboard.py) is always
	// 1024 x 448, so this is a stable compile-time constant pair and
	// does NOT depend on the texture having been loaded yet.  This
	// matters because ATUIVirtualKeyboard_GetDisplayInset() — which
	// drives the emulator viewport — is called from the main loop
	// BEFORE the first ATUIRenderVirtualKeyboard() that would trigger
	// EnsureTexture().
	const float kTexW = 1024.0f;
	const float kTexH = 448.0f;
	float cw = (u1 - u0) * kTexW;
	float ch = (v1 - v0) * kTexH;
	if (ch > 0.0f)
		s_croppedAspect = cw / ch;
}

// Toolbar height (Android only) — scaled by the window display scale
// so CLOSE/ABC buttons stay comfortably finger-sized on high-DPI
// phones.  Returns 0 on desktop.  Shared between ComputeKeyboardRect
// (which reserves this space in the panel) and ATUIRenderVirtualKeyboard
// (which positions the buttons inside it).
static float GetToolbarHeight() {
#ifdef __ANDROID__
	float scale = SDL_GetWindowDisplayScale(g_pWindow);
	if (scale < 1.0f) scale = 1.0f;
	return 56.0f * scale;
#else
	return 0.0f;
#endif
}

// ---------------------------------------------------------------------------
// Placement / layout computation
// ---------------------------------------------------------------------------
static const float kKeyboardAspect = 1024.0f / 448.0f;  // reference aspect ratio

static int ResolveAutoPlacement(int placement) {
	if (placement != kOSKPlacement_Auto)
		return placement;

	int winW, winH;
	GetUIWindowSize(winW, winH);
	float aspect = (winH > 0) ? (float)winW / (float)winH : 1.6f;

	// Landscape windows put the keyboard on the right so the emulator keeps
	// its vertical resolution. Portrait and near-square windows use bottom.
	return (aspect > 1.15f) ? kOSKPlacement_Right : kOSKPlacement_Bottom;
}

static void ComputeKeyboardRect(int placement, ImVec2 *outPos, ImVec2 *outSize) {
	int winW, winH;
	GetUIWindowSize(winW, winH);
	float menuH = g_menuBarHeight;

	int resolved = ResolveAutoPlacement(placement);

#ifdef __ANDROID__
	// On mobile, account for safe-area insets (nav bar, status bar,
	// display cutout) so the keyboard never sits under system UI.
	ATSafeInsets insets = ATAndroid_GetSafeInsets();
	float insetB = (float)insets.bottom;
	float insetR = (float)insets.right;
	float insetL = (float)insets.left;
	float insetT = (float)insets.top;

	// Cropped aspect ratio (key bounding box) gives us a tighter panel
	// that isn't padded by the photo's empty border.  Falls back to the
	// raw keyboard aspect until the texture has loaded and bounds have
	// been computed.
	EnsureKeyBounds();
	const float aspect = s_croppedAspect;
	const float kToolbarH = GetToolbarHeight();

	if (resolved == kOSKPlacement_Right) {
		// Landscape: 50/50 split between screen and keyboard.
		float availH = (float)winH - menuH - insetT - insetB;
		if (availH < 1.0f) availH = 1.0f;
		float availW = (float)winW - insetL - insetR;
		if (availW < 1.0f) availW = 1.0f;

		// Keyboard occupies right 50% of usable width.  The keyboard
		// image area is sized by the *cropped* aspect so the keys fill
		// the panel edge to edge; toolbar chrome sits above it.
		float panelW = availW * 0.5f;
		float kbdAreaH = panelW / aspect;
		float panelH = kbdAreaH + kToolbarH;
		if (panelH > availH) {
			panelH = availH;
			kbdAreaH = panelH - kToolbarH;
			if (kbdAreaH < 1.0f) kbdAreaH = 1.0f;
			panelW = kbdAreaH * aspect;
		}

		outPos->x = (float)winW - insetR - panelW;
		outPos->y = menuH + insetT + (availH - panelH) * 0.5f;
		outSize->x = panelW;
		outSize->y = panelH;
	} else {
		// Portrait: keyboard placed directly below the emulator display,
		// above the bottom safe area.  Keyboard image area gets the
		// cropped aspect; toolbar chrome adds extra height on top.
		float availH = (float)winH - menuH - insetT - insetB;
		if (availH < 1.0f) availH = 1.0f;
		float availW = (float)winW - insetL - insetR;
		if (availW < 1.0f) availW = 1.0f;

		float panelW = availW;
		float kbdAreaH = panelW / aspect;
		float panelH = kbdAreaH + kToolbarH;

		float maxH = availH * 0.50f;
		if (panelH > maxH) {
			panelH = maxH;
			kbdAreaH = panelH - kToolbarH;
			if (kbdAreaH < 1.0f) kbdAreaH = 1.0f;
			panelW = kbdAreaH * aspect;
		}

		// Position above the bottom inset, not at absolute bottom
		outPos->x = insetL + (availW - panelW) * 0.5f;
		outPos->y = (float)winH - insetB - panelH;
		outSize->x = panelW;
		outSize->y = panelH;
	}
#else
	// Desktop: original behavior — full image (no crop) with the
	// baked keyboard aspect ratio.
	if (resolved == kOSKPlacement_Right) {
		float availH = (float)winH - menuH;
		float kbdW = availH * kKeyboardAspect;
		float maxW = (float)winW * 0.4f;
		if (kbdW > maxW) kbdW = maxW;
		float kbdH = kbdW / kKeyboardAspect;

		outPos->x = (float)winW - kbdW;
		outPos->y = menuH + (availH - kbdH) * 0.5f;
		outSize->x = kbdW;
		outSize->y = kbdH;
	} else {
		float kbdH = (float)winW / kKeyboardAspect;
		float maxH = ((float)winH - menuH) * 0.4f;
		if (kbdH > maxH) kbdH = maxH;
		float kbdW = kbdH * kKeyboardAspect;

		outPos->x = ((float)winW - kbdW) * 0.5f;
		outPos->y = (float)winH - kbdH;
		outSize->x = kbdW;
		outSize->y = kbdH;
	}
#endif
}

static void ComputeMobileKeyboardRect(int placement, ImVec2& pos, ImVec2& size);

void ATUIVirtualKeyboard_GetDisplayInset(bool visible, int placement,
	float *outBottom, float *outRight)
{
	if (!visible) {
		s_lastBottomInset = 0;
		s_lastRightInset = 0;
		*outBottom = 0;
		*outRight = 0;
		return;
	}

	ImVec2 pos, size;
	ComputeMobileKeyboardRect(placement, pos, size);

	int winW, winH;
	GetUIWindowSize(winW, winH);

	int resolved = ResolveAutoPlacement(placement);
	if (resolved == kOSKPlacement_Right) {
		s_lastBottomInset = 0;
		// Inset is distance from right edge of window to left edge of keyboard
		s_lastRightInset = (float)winW - pos.x;
	} else {
		// Inset is distance from bottom edge of window to top edge of keyboard
		s_lastBottomInset = (float)winH - pos.y;
		s_lastRightInset = 0;
	}

	*outBottom = s_lastBottomInset;
	*outRight = s_lastRightInset;
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------
//
// Map a key's texture UV (as baked in kOSKKeys) into pixel coordinates
// within the displayed image.  On Android the image shows only the
// cropped sub-region [s_keyU0..s_keyU1] x [s_keyV0..s_keyV1] stretched
// to imgSize; on desktop the image shows the full [0..1] UV range.
static inline void KeyUVToPixels(float ku0, float kv0, float ku1, float kv1,
	ImVec2 imgPos, ImVec2 imgSize,
	float &x0, float &y0, float &x1, float &y1)
{
#ifdef __ANDROID__
	float uRange = s_keyU1 - s_keyU0;
	float vRange = s_keyV1 - s_keyV0;
	if (uRange < 1e-6f) uRange = 1.0f;
	if (vRange < 1e-6f) vRange = 1.0f;
	x0 = imgPos.x + (ku0 - s_keyU0) / uRange * imgSize.x;
	y0 = imgPos.y + (kv0 - s_keyV0) / vRange * imgSize.y;
	x1 = imgPos.x + (ku1 - s_keyU0) / uRange * imgSize.x;
	y1 = imgPos.y + (kv1 - s_keyV0) / vRange * imgSize.y;
#else
	x0 = imgPos.x + ku0 * imgSize.x;
	y0 = imgPos.y + kv0 * imgSize.y;
	x1 = imgPos.x + ku1 * imgSize.x;
	y1 = imgPos.y + kv1 * imgSize.y;
#endif
}

static int HitTestKey(ImVec2 imgPos, ImVec2 imgSize, ImVec2 point) {
	for (int i = 0; i < kOSKKeyCount; i++) {
		float x0, y0, x1, y1;
		KeyUVToPixels(kOSKKeys[i].u0, kOSKKeys[i].v0,
			kOSKKeys[i].u1, kOSKKeys[i].v1,
			imgPos, imgSize, x0, y0, x1, y1);

		if (point.x >= x0 && point.x < x1 && point.y >= y0 && point.y < y1)
			return i;
	}
	return -1;
}

// ---------------------------------------------------------------------------
// Check if a key is a modifier and currently active
// ---------------------------------------------------------------------------
static bool IsModifierActive(int index) {
	if (index < 0 || index >= kOSKKeyCount)
		return false;
	const ATOSKKeyDef &key = kOSKKeys[index];
	if (key.flags & kOSKFlag_Toggle) {
		if (key.scanCode == 0x42)
			return IsShiftActive();
		if (key.scanCode == 0x41)
			return IsControlActive();
	}
	// Console keys show as held while their switch is active
	if (key.flags & kOSKFlag_Console) {
		bool *flag = GetConsoleHeldFlag(key.scanCode);
		return flag && *flag;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Stored each frame so HandleEvent can use the same geometry for touch hit testing
static ImVec2 s_lastImgPos = {0, 0};
static ImVec2 s_lastImgSize = {0, 0};
static bool s_lastVisible = false;

// Close button index — one past the last key
static const int kCloseButtonIndex = 62;  // == kOSKKeyCount
// Text input button index — one past close
static const int kTextInputButtonIndex = 63;

static bool MoveGamepadFocus(int dir) {
	if (s_focusedKey < 0) {
		for (int i = 0; i < kOSKKeyCount; i++) {
			if (kOSKKeys[i].label[0] == 'A' && kOSKKeys[i].label[1] == '\0') {
				s_focusedKey = i;
				break;
			}
		}
		if (s_focusedKey < 0)
			s_focusedKey = 0;
		return true;
	}

	if (s_focusedKey == kCloseButtonIndex) {
#ifdef __ANDROID__
		if (dir == 3)       s_focusedKey = 5;   // down -> ESC
		else if (dir == 1)  s_focusedKey = kTextInputButtonIndex;
		else if (dir == 0)  s_focusedKey = 5;
#else
		if (dir == 3)       s_focusedKey = 5;   // down -> ESC
		else if (dir == 1)  s_focusedKey = 0;
		else if (dir == 0)  s_focusedKey = 5;
#endif
	} else if (s_focusedKey == kTextInputButtonIndex) {
		if (dir == 3)       s_focusedKey = 0;
		else if (dir == 0)  s_focusedKey = kCloseButtonIndex;
		else if (dir == 1)  s_focusedKey = 0;
	} else {
		int next = kOSKKeys[s_focusedKey].nav[dir];
		if (dir == 2 && next < 0)
			s_focusedKey = kCloseButtonIndex;
		else if (next >= 0)
			s_focusedKey = next;
	}

	return true;
}

// Close and text input buttons are drawn as overlays above the keyboard
// image (not embedded in it).  On mobile they are sized to match the
// console buttons (HELP/START); on desktop they use the original small size.
static ImVec2 s_closeBtnMin = {0, 0};
static ImVec2 s_closeBtnMax = {0, 0};
static ImVec2 s_textInputBtnMin = {0, 0};
static ImVec2 s_textInputBtnMax = {0, 0};

static bool HitTestCloseButton(ImVec2 point) {
	return point.x >= s_closeBtnMin.x && point.x < s_closeBtnMax.x
	    && point.y >= s_closeBtnMin.y && point.y < s_closeBtnMax.y;
}

static bool HitTestTextInputButton(ImVec2 point) {
	return point.x >= s_textInputBtnMin.x && point.x < s_textInputBtnMax.x
	    && point.y >= s_textInputBtnMin.y && point.y < s_textInputBtnMax.y;
}

static bool s_wasVisible = false;

static int FindOSKKey(const char *label) {
	for (int i = 0; i < kOSKKeyCount; ++i) {
		if (!strcmp(kOSKKeys[i].label, label))
			return i;
	}
	return -1;
}

static void ComputeMobileKeyboardRect(int placement, ImVec2& pos, ImVec2& size) {
	int winW, winH;
	GetUIWindowSize(winW, winH);
	const int resolved = ResolveAutoPlacement(placement);
	float insetL = 0, insetT = 0, insetR = 0, insetB = 0;
#ifdef __ANDROID__
	const ATSafeInsets insets = ATAndroid_GetSafeInsets();
	insetL = (float)insets.left;
	insetT = (float)insets.top;
	insetR = (float)insets.right;
	insetB = (float)insets.bottom;
#endif
	const float top = g_menuBarHeight + insetT;
	const float availW = (float)winW - insetL - insetR;
	const float availH = (float)winH - top - insetB;

	if (resolved == kOSKPlacement_Right) {
		size.x = availW * 0.48f;
		if (size.x < 420.0f) size.x = std::min(availW, 420.0f);
		if (size.x > 680.0f) size.x = 680.0f;
		size.y = availH;
		pos.x = (float)winW - insetR - size.x;
		pos.y = top;
	} else {
		size.x = availW;
		size.y = availH * 0.58f;
		if (size.y > 480.0f) size.y = 480.0f;
		if (size.y < 300.0f) size.y = std::min(availH, 300.0f);
		pos.x = insetL;
		pos.y = (float)winH - insetB - size.y;
	}
}

static void MobileApplyForcedModifiers(ATSimulator& sim, uint8_t modifiers) {
	if (!modifiers)
		return;
	if (modifiers & 1) s_shiftSticky = true;
	if (modifiers & 2) s_controlSticky = true;
	ApplyModifierState(sim.GetPokey());
}

static void MobilePressKey(ATSimulator& sim, int hitIndex) {
	if (s_mobilePressedOSK >= 0)
		return;
	if (hitIndex < 0 || hitIndex >= (int)s_mobileHits.size())
		return;
	const MobileKey& key = s_mobileHits[hitIndex].key;
	if (key.action != MobileAction::Key)
		return;
	const int osk = FindOSKKey(key.oskLabel);
	if (osk < 0)
		return;

	s_mobilePressedHit = hitIndex;
	s_mobilePressedOSK = osk;
	MobileApplyForcedModifiers(sim, key.forcedModifiers);
	PressKey(sim, osk);
}

static void MobileReleaseKey(ATSimulator& sim) {
	if (s_mobilePressedOSK >= 0)
		ReleaseKey(sim, s_mobilePressedOSK);
	s_mobilePressedHit = -1;
	s_mobilePressedOSK = -1;
}

static void MobileActivateAction(ATSimulator& sim, int hitIndex) {
	if (hitIndex < 0 || hitIndex >= (int)s_mobileHits.size())
		return;
	const MobileKey& key = s_mobileHits[hitIndex].key;
	switch (key.action) {
		case MobileAction::Key:
			MobilePressKey(sim, hitIndex);
			break;
		case MobileAction::Shift: {
			const int osk = FindOSKKey("LSHIFT");
			if (osk >= 0) {
				HandleModifierPress(sim, osk);
				HandleModifierRelease(sim, osk);
			}
			break;
		}
		case MobileAction::Control: {
			const int osk = FindOSKKey("CONTROL");
			if (osk >= 0) {
				HandleModifierPress(sim, osk);
				HandleModifierRelease(sim, osk);
			}
			break;
		}
		case MobileAction::Page:
			s_mobilePage = (s_mobilePage + 1) % 3;
			s_mobileFocus = -1;
			break;
		case MobileAction::Placement:
			if (s_mobilePlacement)
				*s_mobilePlacement = (*s_mobilePlacement + 1) % 3;
			break;
		case MobileAction::NativeText:
			s_nativeTextInputActive = !s_nativeTextInputActive;
#ifdef __ANDROID__
			if (s_nativeTextInputActive)
				SDL_StartTextInput(g_pWindow);
			else
				SDL_StopTextInput(g_pWindow);
#endif
			break;
		case MobileAction::Close:
			s_closeRequested = true;
			break;
	}
}

static ImU32 MobileKeyColor(const MobileKey& key, bool focused, bool pressed) {
	if (pressed) return IM_COL32(221, 91, 42, 255);
	if ((key.action == MobileAction::Shift && IsShiftActive())
		|| (key.action == MobileAction::Control && IsControlActive()))
		return IM_COL32(190, 76, 34, 255);
	if (key.action == MobileAction::Page) return IM_COL32(76, 49, 43, 255);
	if (key.action == MobileAction::Placement) return IM_COL32(47, 63, 82, 255);
	if (key.action == MobileAction::NativeText && s_nativeTextInputActive)
		return IM_COL32(43, 112, 62, 255);
	if (key.action == MobileAction::Close) return IM_COL32(65, 67, 76, 255);
	if (focused) return IM_COL32(62, 72, 88, 255);
	return IM_COL32(49, 52, 62, 255);
}

static void MobileDrawKey(const MobileKey& key, float width, float height) {
	const ImVec2 min = ImGui::GetCursorScreenPos();
	const ImVec2 max(min.x + width, min.y + height);
	const int index = (int)s_mobileHits.size();
	const bool focused = index == s_mobileFocus;
	bool pressed = index == s_mobilePressedHit;
	for (const MobileTouchHold& hold : s_mobileTouchHolds) {
		if (hold.page == s_mobilePage && hold.hitIndex == index) {
			pressed = true;
			break;
		}
	}
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float rounding = std::min(9.0f, height * 0.16f);
	dl->AddRectFilled(min, max, MobileKeyColor(key, focused, pressed), rounding);
	dl->AddRect(min, max, focused ? IM_COL32(0, 200, 255, 255)
		: IM_COL32(74, 78, 91, 255), rounding, 0, focused ? 2.0f : 1.0f);
	const ImVec2 ts = ImGui::CalcTextSize(key.label);
	dl->AddText(ImVec2(min.x + (width - ts.x) * 0.5f,
		min.y + (height - ts.y) * 0.5f), IM_COL32_WHITE, key.label);
	ImGui::PushID(index);
	ImGui::InvisibleButton("##osk", ImVec2(width, height));
	ImGui::PopID();
	s_mobileHits.push_back({min, max, key});
}

static void MobileDrawRow(const MobileKey* keys, const float* weights, int count,
	float width, float height, float gap)
{
	float totalWeight = 0;
	for (int i = 0; i < count; ++i) totalWeight += weights ? weights[i] : 1.0f;
	const float unit = (width - gap * (count - 1)) / totalWeight;
	for (int i = 0; i < count; ++i) {
		if (i) ImGui::SameLine(0, gap);
		MobileDrawKey(keys[i], unit * (weights ? weights[i] : 1.0f), height);
	}
}

#define MK(lbl, osk) { lbl, osk, MobileAction::Key, 0 }
#define MKS(lbl, osk) { lbl, osk, MobileAction::Key, 1 }
#define MKC(lbl, osk) { lbl, osk, MobileAction::Key, 2 }

bool ATUIRenderVirtualKeyboard(ATSimulator &sim, bool visible, int& placement) {
	s_mobilePlacement = &placement;
	if (s_wasVisible && !visible)
		ATUIVirtualKeyboard_ReleaseAll(sim);
	s_wasVisible = visible;
	s_lastVisible = visible;
	if (!visible) return false;

	ImVec2 panelPos, panelSize;
	ComputeMobileKeyboardRect(placement, panelPos, panelSize);
	ImGui::SetNextWindowPos(panelPos);
	ImGui::SetNextWindowSize(panelSize);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7, 7));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5, 5));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.10f, 0.13f, 1));
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
		| ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	if (!ImGui::Begin("##VirtualKeyboardMobile", nullptr, flags)) {
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);
		return false;
	}

	s_mobileHits.clear();
	const float gap = 5.0f;
	const float width = ImGui::GetContentRegionAvail().x;
	const float availableH = ImGui::GetContentRegionAvail().y;
	const int bodyRows = s_mobilePage == 0 ? 5 : (s_mobilePage == 1 ? 4 : 3);
	const int totalRows = bodyRows + 2; // console + persistent navigation
	const float rowUnits = (float)totalRows + (s_mobilePage == 2 ? 0.35f : 0.0f);
	float keyH = (availableH - 24.0f - gap * totalRows) / rowUnits;
	if (keyH > 54.0f) keyH = 54.0f;
	if (keyH < 24.0f) keyH = 24.0f;
	float contentH = 24.0f + keyH * totalRows + gap * totalRows;
	if (s_mobilePage == 2)
		contentH += keyH * 0.35f;
	if (availableH > contentH)
		ImGui::SetCursorPosY(ImGui::GetCursorPosY()
			+ (availableH - contentH) * 0.5f);

	const char* caption = s_mobilePage == 0 ? "ABC"
		: s_mobilePage == 1 ? "SYMBOLS" : "ATARI / EDIT";
	ImGui::TextDisabled("%s", caption);
#ifdef __ANDROID__
	const float toolbarW = 172.0f;
#else
	const float toolbarW = 109.0f;
#endif
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - toolbarW);
	const char* placementLabel = placement == kOSKPlacement_Auto ? "AUTO"
		: placement == kOSKPlacement_Bottom ? "BOTTOM" : "RIGHT";
	const MobileKey placementKey = {
		placementLabel, nullptr, MobileAction::Placement, 0
	};
	MobileDrawKey(placementKey, 52.0f, 22.0f);
#ifdef __ANDROID__
	ImGui::SameLine(0, 5.0f);
	const MobileKey nativeKey = {"PHONE", nullptr, MobileAction::NativeText, 0};
	MobileDrawKey(nativeKey, 58.0f, 22.0f);
#endif
	ImGui::SameLine(0, 5.0f);
	const MobileKey closeKey = {"CLOSE", nullptr, MobileAction::Close, 0};
	MobileDrawKey(closeKey, 52.0f, 22.0f);
	ImGui::NewLine();

	static const MobileKey console[] = {
		MK("HELP", "HELP"), MK("START", "START"), MK("SELECT", "SELECT"),
		MK("OPTION", "OPTION"), MK("RESET", "RESET")
	};
	s_mobileFirstConsole = (int)s_mobileHits.size();
	MobileDrawRow(console, nullptr, 5, width, keyH, gap);

	if (s_mobilePage == 0) {
		static const MobileKey nums[] = {
			MK("1", "1"), MK("2", "2"), MK("3", "3"), MK("4", "4"),
			MK("5", "5"), MK("6", "6"), MK("7", "7"), MK("8", "8"),
			MK("9", "9"), MK("0", "0")
		};
		static const MobileKey qwerty[] = {
			MK("Q", "Q"), MK("W", "W"), MK("E", "E"), MK("R", "R"),
			MK("T", "T"), MK("Y", "Y"), MK("U", "U"), MK("I", "I"),
			MK("O", "O"), MK("P", "P")
		};
		static const MobileKey home[] = {
			MK("A", "A"), MK("S", "S"), MK("D", "D"), MK("F", "F"),
			MK("G", "G"), MK("H", "H"), MK("J", "J"), MK("K", "K"),
			MK("L", "L"), MK("RETURN", "RETURN")
		};
		static const float homeW[] = {1,1,1,1,1,1,1,1,1,1.8f};
		static const MobileKey lower[] = {
			{"SHIFT", "LSHIFT", MobileAction::Shift, 0},
			MK("Z", "Z"), MK("X", "X"), MK("C", "C"), MK("V", "V"),
			MK("B", "B"), MK("N", "N"), MK("M", "M"),
			MK("BK SP", "DELETE")
		};
		static const float lowerW[] = {1.3f,1,1,1,1,1,1,1,1.7f};
		static const MobileKey footer[] = {
			MK(".", "PERIOD"), MKS(":", "COLON"), MK("SPACE", "SPACE")
		};
		static const float footerW[] = {1,1,5};
		MobileDrawRow(nums,nullptr,10,width,keyH,gap);
		MobileDrawRow(qwerty,nullptr,10,width,keyH,gap);
		MobileDrawRow(home,homeW,10,width,keyH,gap);
		MobileDrawRow(lower,lowerW,9,width,keyH,gap);
		MobileDrawRow(footer,footerW,3,width,keyH,gap);
	} else if (s_mobilePage == 1) {
		static const MobileKey r1[] = {
			MKS("!", "1"), MKS("\"", "2"), MKS("#", "3"), MKS("$", "4"),
			MKS("%", "5"), MKS("&", "6"), MKS("'", "7"), MKS("@", "8")
		};
		static const MobileKey r2[] = {
			MKS("(", "9"), MKS(")", "0"), MKS("[", "COMMA"),
			MKS("]", "PERIOD"), MKS("?", "SLASH"), MKS("\\", "LEFT"),
			MKS("^", "RIGHT"), MKS("_", "UP")
		};
		static const MobileKey r3[] = {
			MK("+", "LEFT"), MK("*", "RIGHT"), MK("-", "UP"),
			MK("=", "DOWN"), MKS("|", "DOWN"), MK("/", "SLASH"),
			MK(",", "COMMA"), MK(";", "COLON")
		};
		static const MobileKey r4[] = {
			MK("<", "CLEAR"), MK(">", "INSERT"), MK(".", "PERIOD"),
			MKS(":", "COLON")
		};
		MobileDrawRow(r1,nullptr,8,width,keyH,gap);
		MobileDrawRow(r2,nullptr,8,width,keyH,gap);
		MobileDrawRow(r3,nullptr,8,width,keyH,gap);
		MobileDrawRow(r4,nullptr,4,width,keyH,gap);
	} else {
		static const MobileKey r1[] = {
			MK("ESC", "ESC"), MK("TAB", "TAB"), MK("CAPS", "CAPS"),
			MK("BREAK", "BREAK"), MK("FUJI", "INV")
		};
		static const MobileKey r2[] = {
			MK("<\nSHIFT: CLEAR","CLEAR"),
			MK(">\nSHIFT: INS LINE\nCTRL: INS CHAR","INSERT"),
			MK("BK SP\nSHIFT: DEL LINE\nCTRL: DEL CHAR","DELETE")
		};
		static const MobileKey r3[] = {
			{"SHIFT", "LSHIFT", MobileAction::Shift, 0},
			{"CTRL", "CONTROL", MobileAction::Control, 0},
			MK("SPACE", "SPACE"), MK("RETURN", "RETURN")
		};
		static const float r3w[] = {1.25f,1.25f,3.5f,1.6f};
		MobileDrawRow(r1,nullptr,5,width,keyH,gap);
		MobileDrawRow(r2,nullptr,3,width,keyH * 1.35f,gap);
		MobileDrawRow(r3,r3w,4,width,keyH,gap);
	}

	static const MobileKey nav[] = {
		{"PAGE",nullptr,MobileAction::Page,0}, MKC("LEFT","LEFT"),
		MKC("UP","UP"), MKC("DOWN","DOWN"), MKC("RIGHT","RIGHT")
	};
	MobileKey navKeys[5];
	memcpy(navKeys, nav, sizeof(navKeys));
	navKeys[0].label = s_mobilePage == 0 ? "#+=" : s_mobilePage == 1 ? "ATARI" : "ABC";
	MobileDrawRow(navKeys,nullptr,5,width,keyH,gap);

	if (s_mobileFocus < 0 || s_mobileFocus >= (int)s_mobileHits.size())
		s_mobileFocus = s_mobileFirstConsole;

	// Mouse events arrive through ImGui. Touch is handled from raw SDL finger
	// events below to avoid SDL's synthetic mouse event firing a key twice.
	if (ImGui::GetIO().MouseSource != ImGuiMouseSource_TouchScreen) {
		for (int i = 0; i < (int)s_mobileHits.size(); ++i) {
			const MobileHit& hit = s_mobileHits[i];
			const ImVec2 mouse = ImGui::GetMousePos();
			if (mouse.x < hit.min.x || mouse.x >= hit.max.x
				|| mouse.y < hit.min.y || mouse.y >= hit.max.y)
				continue;
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				s_mobileFocus = i;
				if (hit.key.action == MobileAction::Key) MobilePressKey(sim, i);
				else MobileActivateAction(sim, i);
			}
		}
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) MobileReleaseKey(sim);
	}

	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
	const bool closeRequested = s_closeRequested;
	s_closeRequested = false;
	return closeRequested;
}

#undef MK
#undef MKS
#undef MKC

static bool ATUIRenderVirtualKeyboardLegacy(ATSimulator &sim, bool visible, int placement) {
	// Release all keys when keyboard is hidden (e.g. via menu toggle)
	if (s_wasVisible && !visible)
		ATUIVirtualKeyboard_ReleaseAll(sim);
	s_wasVisible = visible;
	s_lastVisible = visible;

	// Check if close was requested by HandleEvent (gamepad A on close button)
	// before this frame's render.  Reset the flag after reading.
	bool closeFromEvent = s_closeRequested;
	s_closeRequested = false;

	if (!visible) return false;
	if (closeFromEvent) return true;

	ImTextureID texID = EnsureTexture();
	if (!texID) return false;

	ImVec2 panelPos, panelSize;
	ComputeKeyboardRect(placement, &panelPos, &panelSize);

	// Borderless window for the keyboard panel
	ImGui::SetNextWindowPos(panelPos);
	ImGui::SetNextWindowSize(panelSize);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.12f, 1.0f));

	ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

	if (!ImGui::Begin("##VirtualKeyboard", nullptr, flags)) {
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
		return false;
	}

	// On mobile, reserve a toolbar row above the keyboard image for
	// the CLOSE and ABC buttons.  On desktop the buttons overlay the
	// image in the top-left corner so no extra space is needed.
	//
	// On mobile we also crop the keyboard BMP to the actual key
	// bounding box so the photographic border (and the blank "Atari"
	// logo strip on the top-left) don't steal panel area.  The cropped
	// region is stretched to fill panelW x kbdAreaH exactly, since the
	// panel was sized by ComputeKeyboardRect using s_croppedAspect.
#ifdef __ANDROID__
	EnsureKeyBounds();
	const float toolbarH = GetToolbarHeight();
	float kbdAreaH = panelSize.y - toolbarH;
	if (kbdAreaH < 1.0f) kbdAreaH = 1.0f;

	// Image fills the full panel width and the keyboard area below
	// the toolbar.  Panel was sized with s_croppedAspect so this ratio
	// matches the visible UV region and avoids distortion.
	ImVec2 imgSize(panelSize.x, kbdAreaH);
	ImVec2 imgPos(panelPos.x, panelPos.y + toolbarH);

	// Crop UV range — only the actual key area of the texture.
	ImVec2 uv0(s_keyU0, s_keyV0);
	ImVec2 uv1(s_keyU1, s_keyV1);
#else
	// Desktop: full-texture (no crop), centered with aspect-fit.
	float scaleX = panelSize.x / (float)s_texW;
	float scaleY = panelSize.y / (float)s_texH;
	float scale = (scaleX < scaleY) ? scaleX : scaleY;

	ImVec2 imgSize((float)s_texW * scale, (float)s_texH * scale);
	ImVec2 imgPos(panelPos.x + (panelSize.x - imgSize.x) * 0.5f,
	              panelPos.y + (panelSize.y - imgSize.y) * 0.5f);

	ImVec2 uv0(0.0f, 0.0f);
	ImVec2 uv1(1.0f, 1.0f);
#endif

	// Store for touch hit testing in HandleEvent
	s_lastImgPos = imgPos;
	s_lastImgSize = imgSize;

	ImGui::SetCursorScreenPos(imgPos);
	ImGui::Image(texID, imgSize, uv0, uv1);

	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 mousePos = ImGui::GetMousePos();
	bool mouseInWindow = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

	// On Android (and any platform with SDL_HINT_TOUCH_MOUSE_EVENTS),
	// SDL synthesises a mouse-button event for every finger tap.  ImGui's
	// SDL3 backend tags those synthetic events with
	// ImGuiMouseSource_TouchScreen via AddMouseSourceEvent().  The touch
	// path in ATUIVirtualKeyboard_HandleEvent already handles taps via
	// SDL_EVENT_FINGER_DOWN, so honouring the synthetic mouse click here
	// would fire the same action twice — the visible symptom is that the
	// ABC button toggles the native IME on, then immediately back off, so
	// the soft keyboard appears for a single frame and disappears.  Skip
	// the hand-rolled mouse handlers below when the click came from a
	// touch.
	bool ignoreSyntheticMouse =
		(ImGui::GetIO().MouseSource == ImGuiMouseSource_TouchScreen);

	// --- Close button + Text Input button (above keyboard image) ---
	// On mobile, these are sized to match HELP/START console buttons for
	// comfortable touch targets.  On desktop they stay compact.
	{
#ifdef __ANDROID__
		// Buttons sit in the toolbar row above the keyboard image.
		// Sized to be comfortable touch targets (same height as toolbar).
		const float btnH = toolbarH * 0.80f;
		const float btnW = btnH * 2.2f;  // wider than tall for finger target
		const float btnGap = 8.0f;
		const float btnY0 = panelPos.y + (toolbarH - btnH) * 0.5f;
		const float btnY1 = btnY0 + btnH;
		const float btnX0 = panelPos.x + 8.0f;

		s_closeBtnMin = ImVec2(btnX0, btnY0);
		s_closeBtnMax = ImVec2(btnX0 + btnW, btnY1);
		s_textInputBtnMin = ImVec2(btnX0 + btnW + btnGap, btnY0);
		s_textInputBtnMax = ImVec2(btnX0 + btnW + btnGap + btnW, btnY1);
#else
		// Desktop: small close button in top-left of keyboard area.
		// No text input button — zero rect so hit test never matches.
		const float btnH = imgSize.y * 0.09f;
		const float btnW = imgSize.x * 0.065f;
		const float btnY0 = imgPos.y + imgSize.y * 0.01f;
		const float btnY1 = btnY0 + btnH;
		const float btnX0 = imgPos.x + imgSize.x * 0.005f;

		s_closeBtnMin = ImVec2(btnX0, btnY0);
		s_closeBtnMax = ImVec2(btnX0 + btnW, btnY1);
		s_textInputBtnMin = s_textInputBtnMax = ImVec2(0, 0);
#endif

		float rounding = btnH * 0.2f;

		// --- Close button ---
		{
			bool closeFocused = (s_focusedKey == kCloseButtonIndex);
			bool closeHover = mouseInWindow && HitTestCloseButton(mousePos);

			ImU32 closeBg = IM_COL32(80, 80, 80, 160);
			if (closeFocused)
				closeBg = IM_COL32(60, 60, 80, 200);
			else if (closeHover)
				closeBg = IM_COL32(100, 100, 100, 180);
			dl->AddRectFilled(s_closeBtnMin, s_closeBtnMax, closeBg, rounding);

			// "X" icon — centered in the button

			float cx = (s_closeBtnMin.x + s_closeBtnMax.x) * 0.5f;
			float cy = (s_closeBtnMin.y + s_closeBtnMax.y) * 0.5f;
			float halfSz = btnH * 0.25f;
			ImU32 xColor = IM_COL32(220, 220, 220, 240);
			float thick = btnH * 0.08f;
			if (thick < 1.5f) thick = 1.5f;
			dl->AddLine(ImVec2(cx - halfSz, cy - halfSz), ImVec2(cx + halfSz, cy + halfSz), xColor, thick);
			dl->AddLine(ImVec2(cx + halfSz, cy - halfSz), ImVec2(cx - halfSz, cy + halfSz), xColor, thick);

			// Label
			const char *closeLabel = "CLOSE";
			ImVec2 textSize = ImGui::CalcTextSize(closeLabel);
			float tx = cx + halfSz + 4.0f;
			float ty = cy - textSize.y * 0.5f;
			if (tx + textSize.x < s_closeBtnMax.x - 4.0f)
				dl->AddText(ImVec2(tx, ty), IM_COL32(220, 220, 220, 240), closeLabel);

			if (closeFocused)
				dl->AddRect(s_closeBtnMin, s_closeBtnMax, IM_COL32(0, 200, 255, 220), rounding, 0, 2.0f);

			if (mouseInWindow && closeHover && !ignoreSyntheticMouse
				&& ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				s_closeRequested = true;
		}

		// --- Text Input button (opens native mobile keyboard) ---
		// Only shown on Android — desktop has a physical keyboard.
#ifdef __ANDROID__
		{
			bool tiFocused = (s_focusedKey == kTextInputButtonIndex);
			bool tiHover = mouseInWindow && HitTestTextInputButton(mousePos);

			ImU32 tiBg = s_nativeTextInputActive
				? IM_COL32(40, 120, 40, 200)
				: IM_COL32(80, 80, 80, 160);
			if (tiFocused)
				tiBg = IM_COL32(60, 60, 80, 200);
			else if (tiHover)
				tiBg = IM_COL32(100, 100, 100, 180);
			dl->AddRectFilled(s_textInputBtnMin, s_textInputBtnMax, tiBg, rounding);

			// "ABC" label to indicate typing mode
			const char *tiLabel = s_nativeTextInputActive ? "ABC [ON]" : "ABC";
			ImVec2 textSize = ImGui::CalcTextSize(tiLabel);
			float tx = (s_textInputBtnMin.x + s_textInputBtnMax.x) * 0.5f - textSize.x * 0.5f;
			float ty = (s_textInputBtnMin.y + s_textInputBtnMax.y) * 0.5f - textSize.y * 0.5f;
			dl->AddText(ImVec2(tx, ty), IM_COL32(220, 220, 220, 240), tiLabel);

			if (tiFocused)
				dl->AddRect(s_textInputBtnMin, s_textInputBtnMax, IM_COL32(0, 200, 255, 220), rounding, 0, 2.0f);

			if (mouseInWindow && tiHover && !ignoreSyntheticMouse
				&& ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				s_nativeTextInputActive = !s_nativeTextInputActive;
				if (s_nativeTextInputActive)
					SDL_StartTextInput(g_pWindow);
				else
					SDL_StopTextInput(g_pWindow);
			}
		}
#endif
	}

	// --- Key overlays ---
	s_hoverKey = -1;
	if (mouseInWindow)
		s_hoverKey = HitTestKey(imgPos, imgSize, mousePos);

	for (int i = 0; i < kOSKKeyCount; i++) {
		float x0, y0, x1, y1;
		KeyUVToPixels(kOSKKeys[i].u0, kOSKKeys[i].v0,
			kOSKKeys[i].u1, kOSKKeys[i].v1,
			imgPos, imgSize, x0, y0, x1, y1);
		ImVec2 keyMin(x0, y0);
		ImVec2 keyMax(x1, y1);

		bool isPressed = (s_pressedKey == i);
		bool isFocused = (s_focusedKey == i);
		bool isHover = (s_hoverKey == i);
		bool isModActive = IsModifierActive(i);

		if (isPressed || isModActive)
			dl->AddRectFilled(keyMin, keyMax, IM_COL32(255, 255, 255, 140), 3.0f);
		else if (isHover || isFocused)
			dl->AddRectFilled(keyMin, keyMax, IM_COL32(255, 255, 255, 80), 3.0f);

		// Gamepad focus ring
		if (isFocused)
			dl->AddRect(keyMin, keyMax, IM_COL32(0, 200, 255, 220), 3.0f, 0, 2.0f);
	}

	// Handle mouse click on keys (desktop / external mouse).  Touch-tap
	// presses are routed via the SDL_EVENT_FINGER_DOWN path in
	// ATUIVirtualKeyboard_HandleEvent — we must not also fire here off
	// the synthetic mouse click that SDL generates from the same touch,
	// or the key would be pressed twice and lift events would race.
	if (mouseInWindow && !ignoreSyntheticMouse) {
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && s_hoverKey >= 0)
			PressKey(sim, s_hoverKey);
	}
	if (!ignoreSyntheticMouse
		&& ImGui::IsMouseReleased(ImGuiMouseButton_Left) && s_pressedKey >= 0) {
		const ATOSKKeyDef &key = kOSKKeys[s_pressedKey];
		if (key.flags & kOSKFlag_Toggle) {
			HandleModifierRelease(sim, s_pressedKey);
			s_pressedKey = -1;
		} else {
			ReleaseKey(sim, s_pressedKey);
		}
	}

	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();

	return s_closeRequested;
}

// ---------------------------------------------------------------------------
// Event handling (gamepad + touch)
// ---------------------------------------------------------------------------
static int MobileHitTest(float x, float y) {
	for (int i = 0; i < (int)s_mobileHits.size(); ++i) {
		const MobileHit& hit = s_mobileHits[i];
		if (x >= hit.min.x && x < hit.max.x && y >= hit.min.y && y < hit.max.y)
			return i;
	}
	return -1;
}

static bool MobileMoveFocus(int dir) {
	if (s_mobileHits.empty())
		return true;
	if (s_mobileFocus < 0 || s_mobileFocus >= (int)s_mobileHits.size()) {
		s_mobileFocus = 0;
		return true;
	}
	const MobileHit& from = s_mobileHits[s_mobileFocus];
	const float fx = (from.min.x + from.max.x) * 0.5f;
	const float fy = (from.min.y + from.max.y) * 0.5f;
	int best = -1;
	float bestScore = FLT_MAX;
	for (int i = 0; i < (int)s_mobileHits.size(); ++i) {
		if (i == s_mobileFocus) continue;
		const MobileHit& to = s_mobileHits[i];
		const float dx = (to.min.x + to.max.x) * 0.5f - fx;
		const float dy = (to.min.y + to.max.y) * 0.5f - fy;
		if ((dir == 0 && dx >= -1) || (dir == 1 && dx <= 1)
			|| (dir == 2 && dy >= -1) || (dir == 3 && dy <= 1))
			continue;
		const float primary = dir < 2 ? fabsf(dx) : fabsf(dy);
		const float cross = dir < 2 ? fabsf(dy) : fabsf(dx);
		const float score = primary + cross * 2.5f;
		if (score < bestScore) { bestScore = score; best = i; }
	}
	if (best >= 0) s_mobileFocus = best;
	return true;
}

bool ATUIVirtualKeyboard_HandleEvent(const SDL_Event &ev, ATSimulator &sim, bool visible) {
	if (!visible)
		return false;

	// Physical keyboard navigation is useful in Desktop Mode and on TV-style
	// systems with a wireless keyboard. Keep ordinary typing keys routed to the
	// Atari; only conventional UI navigation keys are intercepted here.
	if (ev.type == SDL_EVENT_KEY_DOWN) {
		switch (ev.key.key) {
			case SDLK_LEFT:  return MobileMoveFocus(0);
			case SDLK_RIGHT: return MobileMoveFocus(1);
			case SDLK_UP:    return MobileMoveFocus(2);
			case SDLK_DOWN:  return MobileMoveFocus(3);
			case SDLK_RETURN:
			case SDLK_KP_ENTER:
				if (!ev.key.repeat && s_mobileFocus >= 0
					&& s_mobileFocus < (int)s_mobileHits.size())
					MobileActivateAction(sim, s_mobileFocus);
				return true;
			case SDLK_PAGEUP:
				if (!ev.key.repeat) {
					s_mobilePage = (s_mobilePage + 2) % 3;
					s_mobileFocus = -1;
				}
				return true;
			case SDLK_PAGEDOWN:
				if (!ev.key.repeat) {
					s_mobilePage = (s_mobilePage + 1) % 3;
					s_mobileFocus = -1;
				}
				return true;
			case SDLK_ESCAPE:
				s_closeRequested = true;
				return true;
			default:
				break;
		}
	}
	if (ev.type == SDL_EVENT_KEY_UP) {
		switch (ev.key.key) {
			case SDLK_RETURN:
			case SDLK_KP_ENTER:
				MobileReleaseKey(sim);
				return true;
			case SDLK_LEFT:
			case SDLK_RIGHT:
			case SDLK_UP:
			case SDLK_DOWN:
			case SDLK_PAGEUP:
			case SDLK_PAGEDOWN:
			case SDLK_ESCAPE:
				return true;
			default:
				break;
		}
	}

	if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
		switch (ev.gbutton.button) {
			case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  return MobileMoveFocus(0);
			case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return MobileMoveFocus(1);
			case SDL_GAMEPAD_BUTTON_DPAD_UP:    return MobileMoveFocus(2);
			case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  return MobileMoveFocus(3);
			case SDL_GAMEPAD_BUTTON_SOUTH:
				if (s_mobileFocus >= 0 && s_mobileFocus < (int)s_mobileHits.size())
					MobileActivateAction(sim, s_mobileFocus);
				return true;
			case SDL_GAMEPAD_BUTTON_WEST:
				s_mobilePage = (s_mobilePage + 1) % 3;
				s_mobileFocus = -1;
				return true;
			case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
				s_shiftHeld = true;
				ApplyModifierState(sim.GetPokey());
				return true;
			case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
				s_controlHeld = true;
				ApplyModifierState(sim.GetPokey());
				return true;
			default: break;
		}
	}
	if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
		switch (ev.gbutton.button) {
			case SDL_GAMEPAD_BUTTON_SOUTH:
				MobileReleaseKey(sim);
				return true;
			case SDL_GAMEPAD_BUTTON_WEST:
				return true;
			case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
				s_shiftHeld = false;
				ApplyModifierState(sim.GetPokey());
				return true;
			case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
				s_controlHeld = false;
				ApplyModifierState(sim.GetPokey());
				return true;
			default: break;
		}
	}
	if (ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION
		&& (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX
			|| ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
		if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) s_gamepadNavAxisX = ev.gaxis.value;
		else s_gamepadNavAxisY = ev.gaxis.value;
		constexpr int threshold = 18000;
		int dir = -1;
		if (abs(s_gamepadNavAxisX) > threshold || abs(s_gamepadNavAxisY) > threshold) {
			if (abs(s_gamepadNavAxisX) >= abs(s_gamepadNavAxisY)) dir = s_gamepadNavAxisX < 0 ? 0 : 1;
			else dir = s_gamepadNavAxisY < 0 ? 2 : 3;
		}
		if (dir != s_gamepadNavDir) {
			s_gamepadNavDir = dir;
			if (dir >= 0) MobileMoveFocus(dir);
		}
		return dir >= 0;
	}

	if (ev.type == SDL_EVENT_FINGER_DOWN && s_lastVisible) {
		int winW, winH;
		GetUIWindowSize(winW, winH);
		const int hit = MobileHitTest(ev.tfinger.x * winW, ev.tfinger.y * winH);
		if (hit >= 0) {
			s_mobileFocus = hit;
			const MobileKey& key = s_mobileHits[hit].key;
			if (key.action == MobileAction::Key) {
				const int osk = FindOSKKey(key.oskLabel);
				if (osk >= 0) {
					MobileApplyForcedModifiers(sim, key.forcedModifiers);
					PressKey(sim, osk);
					s_mobileTouchHolds.push_back({
						ev.tfinger.fingerID, osk, hit, s_mobilePage
					});
				}
			} else {
				MobileActivateAction(sim, hit);
			}
			return true;
		}
	}
	if (ev.type == SDL_EVENT_FINGER_UP || ev.type == SDL_EVENT_FINGER_CANCELED) {
		for (auto it = s_mobileTouchHolds.begin();
			it != s_mobileTouchHolds.end(); ++it)
		{
			if (it->finger != ev.tfinger.fingerID)
				continue;
			if (it->oskIndex >= 0)
				ReleaseKey(sim, it->oskIndex);
			s_mobileTouchHolds.erase(it);
			return true;
		}
	}
	return false;
}

static bool ATUIVirtualKeyboard_HandleEventLegacy(const SDL_Event &ev, ATSimulator &sim, bool visible) {
	if (!visible)
		return false;

	// --- Gamepad navigation ---
	if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
		switch (ev.gbutton.button) {
			case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
			case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
			case SDL_GAMEPAD_BUTTON_DPAD_UP:
			case SDL_GAMEPAD_BUTTON_DPAD_DOWN: {
				int dir;
				switch (ev.gbutton.button) {
					case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  dir = 0; break;
					case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: dir = 1; break;
					case SDL_GAMEPAD_BUTTON_DPAD_UP:    dir = 2; break;
					case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  dir = 3; break;
					default: dir = 0; break;
				}

				return MoveGamepadFocus(dir);
			}

			case SDL_GAMEPAD_BUTTON_SOUTH:  // A — press focused key
				if (s_focusedKey == kCloseButtonIndex) {
					s_closeRequested = true;
					return true;
				}
				if (s_focusedKey == kTextInputButtonIndex) {
					s_nativeTextInputActive = !s_nativeTextInputActive;
#ifdef __ANDROID__
					if (s_nativeTextInputActive)
						SDL_StartTextInput(g_pWindow);
					else
						SDL_StopTextInput(g_pWindow);
#endif
					return true;
				}
				if (s_focusedKey >= 0 && s_focusedKey < kOSKKeyCount)
					PressKey(sim, s_focusedKey);
				return true;

			case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  // LB — hold Shift
				s_shiftHeld = true;
				sim.GetPokey().SetShiftKeyState(true, !g_kbdOpts.mbFullRawKeys);
				return true;

			case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:  // RB — hold Control
				s_controlHeld = true;
				sim.GetPokey().SetControlKeyState(true);
				return true;

			default:
				break;
		}
	}

	if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
		switch (ev.gbutton.button) {
			case SDL_GAMEPAD_BUTTON_SOUTH:
				if (s_pressedKey >= 0)
					ReleaseKey(sim, s_pressedKey);
				return true;

			case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
				s_shiftHeld = false;
				if (!s_shiftSticky)
					sim.GetPokey().SetShiftKeyState(false, !g_kbdOpts.mbFullRawKeys);
				return true;

			case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
				s_controlHeld = false;
				if (!s_controlSticky)
					sim.GetPokey().SetControlKeyState(false);
				return true;

			default:
				break;
		}
	}

	if (ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION
		&& (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX
			|| ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY))
	{
		if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX)
			s_gamepadNavAxisX = ev.gaxis.value;
		else
			s_gamepadNavAxisY = ev.gaxis.value;

		constexpr int kNavThreshold = 18000;
		int dir = -1;
		if (abs(s_gamepadNavAxisX) > kNavThreshold
			|| abs(s_gamepadNavAxisY) > kNavThreshold) {
			if (abs(s_gamepadNavAxisX) >= abs(s_gamepadNavAxisY))
				dir = s_gamepadNavAxisX < 0 ? 0 : 1;
			else
				dir = s_gamepadNavAxisY < 0 ? 2 : 3;
		}

		if (dir != s_gamepadNavDir) {
			s_gamepadNavDir = dir;
			if (dir >= 0)
				return MoveGamepadFocus(dir);
		}

		return dir >= 0;
	}

	// --- Touch events (mobile) ---
	if (ev.type == SDL_EVENT_FINGER_DOWN) {
		if (!s_lastVisible)
			return false;

		int winW, winH;
		GetUIWindowSize(winW, winH);
		float fx = ev.tfinger.x * (float)winW;
		float fy = ev.tfinger.y * (float)winH;
		ImVec2 touchPt(fx, fy);

		// Check close button first
		if (HitTestCloseButton(touchPt)) {
			s_closeRequested = true;
			return true;
		}

		// Check text input button
		if (HitTestTextInputButton(touchPt)) {
			s_nativeTextInputActive = !s_nativeTextInputActive;
#ifdef __ANDROID__
			if (s_nativeTextInputActive)
				SDL_StartTextInput(g_pWindow);
			else
				SDL_StopTextInput(g_pWindow);
			ATAndroid_Vibrate(10);
#endif
			return true;
		}

		int hit = HitTestKey(s_lastImgPos, s_lastImgSize, touchPt);
		if (hit >= 0) {
			s_touchFinger = ev.tfinger.fingerID;
			s_touchActive = true;
			PressKey(sim, hit);
			return true;
		}
	}

	if (ev.type == SDL_EVENT_FINGER_UP) {
		if (s_touchActive && ev.tfinger.fingerID == s_touchFinger) {
			if (s_pressedKey >= 0) {
				const ATOSKKeyDef &key = kOSKKeys[s_pressedKey];
				if (key.flags & kOSKFlag_Toggle)
					HandleModifierRelease(sim, s_pressedKey);
				else
					ReleaseKey(sim, s_pressedKey);
				s_pressedKey = -1;
			}
			s_touchActive = false;
			return true;
		}
	}

	return false;
}

bool ATUIVirtualKeyboard_IsNativeTextInputActive() {
	return s_nativeTextInputActive;
}
