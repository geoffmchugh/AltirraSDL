//	AltirraSDL - Shared display backend preference helpers

#include <stdafx.h>
#include "display_backend.h"

#include <SDL3/SDL_stdinc.h>
#include <vd2/system/registry.h>

namespace {

constexpr const char *kRegistryKey = "SDL3";
constexpr const char *kRegistryValue = "Display backend";

} // namespace

const char *ATDisplayBackendPreferenceName(
	ATDisplayBackendPreference preference)
{
	switch (preference) {
	case ATDisplayBackendPreference::SDLGPU:
		return "sdlgpu";
	case ATDisplayBackendPreference::OpenGL:
		return "opengl";
	case ATDisplayBackendPreference::SDLRenderer:
		return "sdlrenderer";
	}

	return "sdlgpu";
}

bool ATDisplayBackendPreferenceParse(const char *name,
	ATDisplayBackendPreference& preference)
{
	if (!name)
		return false;

	if (!SDL_strcasecmp(name, "sdlgpu") || !SDL_strcasecmp(name, "gpu")) {
		preference = ATDisplayBackendPreference::SDLGPU;
		return true;
	}

	if (!SDL_strcasecmp(name, "opengl") || !SDL_strcasecmp(name, "gl")) {
		preference = ATDisplayBackendPreference::OpenGL;
		return true;
	}

	if (!SDL_strcasecmp(name, "sdlrenderer")
		|| !SDL_strcasecmp(name, "renderer")
		|| !SDL_strcasecmp(name, "compatible"))
	{
		preference = ATDisplayBackendPreference::SDLRenderer;
		return true;
	}

	return false;
}

ATDisplayBackendPreference ATDisplayBackendPreferenceLoad() {
	VDRegistryAppKey key(kRegistryKey, false);
	const int value = key.getInt(kRegistryValue,
		(int)ATDisplayBackendPreference::SDLGPU);

	if (value < (int)ATDisplayBackendPreference::SDLGPU
		|| value > (int)ATDisplayBackendPreference::SDLRenderer)
	{
		return ATDisplayBackendPreference::SDLGPU;
	}

	return (ATDisplayBackendPreference)value;
}

void ATDisplayBackendPreferenceSave(ATDisplayBackendPreference preference) {
	if (preference < ATDisplayBackendPreference::SDLGPU
		|| preference > ATDisplayBackendPreference::SDLRenderer)
	{
		preference = ATDisplayBackendPreference::SDLGPU;
	}

	VDRegistryAppKey key(kRegistryKey, true);
	key.setInt(kRegistryValue, (int)preference);

	extern void ATRegistryFlushToDisk();
	ATRegistryFlushToDisk();
}

const char *ATDisplayBackendTypeName(DisplayBackendType type) {
	switch (type) {
	case DisplayBackendType::SDLGPU:
		return "SDL_GPU";
	case DisplayBackendType::OpenGL:
		return "OpenGL";
	case DisplayBackendType::SDLRenderer:
		return "SDL_Renderer";
	}

	return "Unknown";
}
