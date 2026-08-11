// AltirraSDL macOS leak diagnostic implementation.
// See MAC_OS_LEAK_DEBUG.md for build, test, and removal instructions.

#include <stdafx.h>
#include "macos_leak_debug.h"

#ifdef ALTIRRA_MAC_OS_LEAK_DEBUG

#include <SDL3/SDL.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

SDL_Window *g_window;
const char *g_backend = "unknown";
uint64_t g_startTicks;
uint64_t g_lastReportTicks;
uint64_t g_emulatedFrames;
uint64_t g_renderCalls;
uint64_t g_uploadCalls;
uint64_t g_uploadBytes;
bool g_paused;
bool g_newFramesOnly;
bool g_disableUpload;
bool g_disablePresent;

bool ReadFlag(const char *name) {
	const char *value = SDL_getenv(name);
	return value && *value && strcmp(value, "0") && SDL_strcasecmp(value, "false");
}

void ReadMemory(uint64_t& footprint, uint64_t& resident, uint64_t& virtualSize,
	uint64_t& compressed)
{
	footprint = resident = virtualSize = compressed = 0;
	task_vm_info_data_t vmInfo {};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO,
		reinterpret_cast<task_info_t>(&vmInfo), &count) == KERN_SUCCESS)
	{
		footprint = vmInfo.phys_footprint;
		resident = vmInfo.resident_size;
		virtualSize = vmInfo.virtual_size;
		compressed = vmInfo.compressed;
	}
}

} // namespace

void ATMacLeakDebugInit(SDL_Window *window, const char *backend) {
	g_window = window;
	g_backend = backend ? backend : "unknown";
	g_startTicks = g_lastReportTicks = SDL_GetTicks();
	g_newFramesOnly = ReadFlag("ALTIRRA_MAC_LEAK_NEW_FRAMES_ONLY");
	g_disableUpload = ReadFlag("ALTIRRA_MAC_LEAK_DISABLE_UPLOAD");
	g_disablePresent = ReadFlag("ALTIRRA_MAC_LEAK_DISABLE_PRESENT");

	fprintf(stderr,
		"MACLEAK START backend=%s new_frames_only=%d disable_upload=%d "
		"disable_present=%d interval_seconds=10\n",
		g_backend, g_newFramesOnly, g_disableUpload, g_disablePresent);
	fprintf(stderr,
		"MACLEAK COLUMNS elapsed_s footprint_mb resident_mb virtual_mb "
		"compressed_mb malloc_used_mb malloc_allocated_mb paused hidden "
		"minimized emu_frames renders uploads upload_mb\n");
	fflush(stderr);
}

void ATMacLeakDebugOnEmulatedFrame() {
	++g_emulatedFrames;
}

void ATMacLeakDebugOnRender() {
	++g_renderCalls;
}

bool ATMacLeakDebugShouldUpload(bool newFrame) {
	return !g_disableUpload && (!g_newFramesOnly || newFrame);
}

void ATMacLeakDebugOnUpload(size_t bytes) {
	++g_uploadCalls;
	g_uploadBytes += bytes;
}

bool ATMacLeakDebugShouldPresent() {
	return !g_disablePresent;
}

void ATMacLeakDebugSetPaused(bool paused) {
	g_paused = paused;
}

void ATMacLeakDebugTick() {
	const uint64_t now = SDL_GetTicks();
	if (now - g_lastReportTicks < 10000)
		return;
	g_lastReportTicks = now;

	uint64_t footprint, resident, virtualSize, compressed;
	ReadMemory(footprint, resident, virtualSize, compressed);

	malloc_statistics_t mallocStats {};
	malloc_zone_statistics(malloc_default_zone(), &mallocStats);

	const SDL_WindowFlags flags = g_window ? SDL_GetWindowFlags(g_window) : 0;
	constexpr double mib = 1024.0 * 1024.0;
	fprintf(stderr,
		"MACLEAK DATA %.1f %.1f %.1f %.1f %.1f %.1f %.1f %d %d %d "
		"%llu %llu %llu %.1f\n",
		(now - g_startTicks) / 1000.0,
		footprint / mib, resident / mib, virtualSize / mib, compressed / mib,
		mallocStats.size_in_use / mib, mallocStats.size_allocated / mib,
		g_paused,
		(flags & SDL_WINDOW_HIDDEN) != 0,
		(flags & SDL_WINDOW_MINIMIZED) != 0,
		static_cast<unsigned long long>(g_emulatedFrames),
		static_cast<unsigned long long>(g_renderCalls),
		static_cast<unsigned long long>(g_uploadCalls),
		g_uploadBytes / mib);
	fflush(stderr);
}

#endif
