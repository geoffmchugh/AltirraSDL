//	AltirraSDL - persisted input-map selections

#include <stdafx.h>

#include "input_selection.h"

#include "inputmanager.h"
#include "inputmap.h"
#include "inputdefs.h"
#include "settings.h"
#include "simulator.h"

#include <vd2/system/refcount.h>
#include <vd2/system/registry.h>
#include <vd2/system/VDString.h>

#include <algorithm>
#include <cwctype>

extern ATSimulator g_sim;
extern void ATRegistryFlushToDisk();

#if defined(__EMSCRIPTEN__)
extern "C" void ATWasmSyncFSOut();
#endif

namespace ATInputSelection {

namespace {

bool ContainsCI(const wchar_t *haystack, const wchar_t *needle) {
	if (!haystack || !needle || !*needle)
		return false;

	for (; *haystack; ++haystack) {
		size_t i = 0;
		while (needle[i] && haystack[i]
			&& std::towlower((wint_t)haystack[i])
				== std::towlower((wint_t)needle[i]))
		{
			++i;
		}

		if (!needle[i])
			return true;
	}

	return false;
}

bool HasSavedSelection() {
	// Match ATExchangeSettings(): a child only supplies Input when that
	// category is both enabled and saved; otherwise it inherits from the
	// first ancestor that does.  Looking only at the current profile would
	// seed defaults over an inherited user selection.
	uint32 profileId = ATSettingsGetCurrentProfileId();
	vdfastvector<uint32> seenProfiles;

	for (;;) {
		if (std::find(seenProfiles.begin(), seenProfiles.end(), profileId)
			!= seenProfiles.end())
		{
			profileId = 0;
		}
		seenProfiles.push_back(profileId);

		const ATSettingsCategory enabled =
			ATSettingsProfileGetCategoryMask(profileId);
		const ATSettingsCategory saved =
			ATSettingsProfileGetSavedCategoryMask(profileId);
		if ((enabled & kATSettingsCategory_Input)
			&& (saved & kATSettingsCategory_Input))
		{
			VDStringA path;
			path.sprintf("Profiles\\%08X", profileId);
			VDRegistryAppKey key(path.c_str(), false);
			return key.isReady()
				&& key.getValueType("Input: Active map names")
					== VDRegistryKey::kTypeString;
		}

		if (!profileId)
			return false;

		profileId = ATSettingsProfileGetParent(profileId);
	}
}

bool IsDefaultPort1Map(const ATInputMap& map, bool is5200) {
	if (!map.UsesPhysicalPort(0) || map.GetSpecificInputUnit() != -1)
		return false;

	const ATInputControllerType type = is5200
		? kATInputControllerType_5200Controller
		: kATInputControllerType_Joystick;
	if (!map.HasControllerType(type))
		return false;

	const wchar_t *name = map.GetName();
	if (is5200) {
		// Do not enable both keyboard modes: they bind the same keys with
		// incompatible absolute/relative semantics.  Use one keyboard map
		// and one game-controller map so the sources compose cleanly.
		return (ContainsCI(name, L"Keyboard")
				&& ContainsCI(name, L"absolute"))
			|| ContainsCI(name, L"Xbox 360 Controller");
	}

	return ContainsCI(name, L"Arrow Keys")
		|| ContainsCI(name, L"Numpad")
		|| ContainsCI(name, L"Gamepad");
}

void Flush() {
	if (ATSettingsGetTemporaryProfileMode())
		return;

	ATRegistryFlushToDisk();
#if defined(__EMSCRIPTEN__)
	ATWasmSyncFSOut();
#endif
}

} // namespace

void CommitSelections() {
	if (ATSettingsGetTemporaryProfileMode()
		|| ATSettingsGetBootstrapProfileMode())
		return;

	ATSaveSettings(kATSettingsCategory_Input);
	Flush();
}

void CommitMapsAndSelections() {
	if (ATSettingsGetTemporaryProfileMode()
		|| ATSettingsGetBootstrapProfileMode())
		return;

	ATSaveSettings((ATSettingsCategory)(kATSettingsCategory_InputMaps
		| kATSettingsCategory_Input));
	Flush();
}

void AssignUniqueName(ATInputManager& im, ATInputMap& map,
	const wchar_t *requestedName)
{
	VDStringW base(requestedName && *requestedName
		? requestedName : L"Input map");
	// The shared selection format uses one map name per line.
	for (wchar_t& c : base) {
		if (c == L'\r' || c == L'\n')
			c = L' ';
	}
	VDStringW candidate(base);

	for (uint32 suffix = 2;; ++suffix) {
		bool exists = false;
		const uint32 count = im.GetInputMapCount();
		for (uint32 i = 0; i < count; ++i) {
			vdrefptr<ATInputMap> other;
			if (im.GetInputMapByIndex(i, ~other) && other
				&& other.get() != &map
				&& !wcscmp(other->GetName(), candidate.c_str()))
			{
				exists = true;
				break;
			}
		}

		if (!exists) {
			map.SetName(candidate.c_str());
			return;
		}

		candidate.sprintf(L"%ls (%u)", base.c_str(), suffix);
	}
}

void NormalizeMapNames(ATInputManager& im) {
	if (ATSettingsGetTemporaryProfileMode()
		|| ATSettingsGetBootstrapProfileMode())
		return;

	bool changed = false;
	const uint32 count = im.GetInputMapCount();
	for (uint32 i = 0; i < count; ++i) {
		vdrefptr<ATInputMap> map;
		if (!im.GetInputMapByIndex(i, ~map) || !map)
			continue;

		VDStringW oldName(map->GetName());
		AssignUniqueName(im, *map, oldName.c_str());
		changed |= oldName != map->GetName();
	}

	if (changed)
		CommitMapsAndSelections();
}

void Toggle(ATInputManager& im, ATInputMap *map) {
	if (!map)
		return;

	im.ActivateInputMap(map, !im.IsInputMapEnabled(map));
	CommitSelections();
}

void ClearPort(ATInputManager& im, int portIdx) {
	const uint32 count = im.GetInputMapCount();

	for (uint32 i = 0; i < count; ++i) {
		vdrefptr<ATInputMap> map;
		if (im.GetInputMapByIndex(i, ~map) && map
			&& map->UsesPhysicalPort(portIdx)
			&& im.IsInputMapEnabled(map))
		{
			im.ActivateInputMap(map, false);
		}
	}

	// Persist even when the port was already empty: selecting "None" is
	// an explicit choice that must prevent first-run defaults on restart.
	CommitSelections();
}

void SeedDefaultsIfNoSelection() {
	if (ATSettingsGetTemporaryProfileMode()
		|| ATSettingsGetBootstrapProfileMode()
		|| HasSavedSelection())
		return;

	ATInputManager *im = g_sim.GetInputManager();
	if (!im)
		return;

	const bool is5200 = g_sim.GetHardwareMode() == kATHardwareMode_5200;
	const uint32 count = im->GetInputMapCount();

	// LoadSelections() chooses a single 5200 map when no state exists.
	// Replace that fallback with the SDL multi-source first-run default.
	for (uint32 i = 0; i < count; ++i) {
		vdrefptr<ATInputMap> map;
		if (im->GetInputMapByIndex(i, ~map) && map
			&& map->UsesPhysicalPort(0))
		{
			im->ActivateInputMap(map, false);
		}
	}

	for (uint32 i = 0; i < count; ++i) {
		vdrefptr<ATInputMap> map;
		if (im->GetInputMapByIndex(i, ~map) && map
			&& IsDefaultPort1Map(*map, is5200))
		{
			im->ActivateInputMap(map, true);
		}
	}

	// Persist even if no preset matched.  This records an explicit empty
	// selection rather than trying to seed again on every launch.
	CommitSelections();
}

} // namespace ATInputSelection
