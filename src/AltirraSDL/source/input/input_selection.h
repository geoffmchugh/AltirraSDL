//	AltirraSDL - persisted input-map selections
//
//	The enabled input-map set is the one source of truth for every UI
//	surface.  It is deliberately independent of Desktop/Gaming mode.

#pragma once

class ATInputManager;
class ATInputMap;

namespace ATInputSelection {

// Toggle one map without changing any other map.  The new selection is
// immediately written to the current settings profile.
void Toggle(ATInputManager& im, ATInputMap *map);

// Disable every map that contributes to the given physical Atari port.
void ClearPort(ATInputManager& im, int portIdx);

// Persist active-map flags.  Use this after a UI action that only changes
// map enablement; map definitions themselves are unchanged.
void CommitSelections();

// Persist both map definitions and their active-map flags.  Used by the
// Input Mappings editor after creating, editing, cloning, or deleting maps.
void CommitMapsAndSelections();

// Input selections are serialized by map name in the shared Altirra format.
// Keep names unique so two maps can retain independent enabled states.
void AssignUniqueName(ATInputManager& im, ATInputMap& map,
	const wchar_t *requestedName);
void NormalizeMapNames(ATInputManager& im);

// First-run-only default.  If the active profile has no saved active-map
// value, seed its sensible multi-source Port-1 defaults once and persist
// them.  An explicitly saved empty value means "None" and is never seeded.
void SeedDefaultsIfNoSelection();

} // namespace ATInputSelection
