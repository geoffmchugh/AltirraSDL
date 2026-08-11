// AltirraSDL macOS leak diagnostic hooks.
//
// All implementation is compiled out unless ALTIRRA_MAC_OS_LEAK_DEBUG is
// defined. See MAC_OS_LEAK_DEBUG.md before changing or removing these hooks.

#pragma once

#include <stddef.h>

struct SDL_Window;

#ifdef ALTIRRA_MAC_OS_LEAK_DEBUG
void ATMacLeakDebugInit(SDL_Window *window, const char *backend);
void ATMacLeakDebugOnEmulatedFrame();
void ATMacLeakDebugOnRender();
bool ATMacLeakDebugShouldUpload(bool newFrame);
void ATMacLeakDebugOnUpload(size_t bytes);
bool ATMacLeakDebugShouldPresent();
void ATMacLeakDebugSetPaused(bool paused);
void ATMacLeakDebugTick();
#else
inline void ATMacLeakDebugInit(SDL_Window *, const char *) {}
inline void ATMacLeakDebugOnEmulatedFrame() {}
inline void ATMacLeakDebugOnRender() {}
inline bool ATMacLeakDebugShouldUpload(bool) { return true; }
inline void ATMacLeakDebugOnUpload(size_t) {}
inline bool ATMacLeakDebugShouldPresent() { return true; }
inline void ATMacLeakDebugSetPaused(bool) {}
inline void ATMacLeakDebugTick() {}
#endif
