//	AltirraSDL - Game metadata scraper settings
//
//	Machine-global configuration for the online metadata feature: which
//	provider account to use, what to download, and region/language
//	preferences.
//
//	Storage rule (CLAUDE.md, "Persisting fork-only settings"): none of
//	this is meaningful per emulation profile — credentials, download
//	toggles and a region preference belong to the installation, not to
//	the machine being emulated — so it lives in a fork-owned
//	VDRegistryAppKey namespace ("Metadata"), exactly like the Game
//	Library's own "GameLibrary" key, and NOT in settings.cpp or the
//	ui_state_settings.cpp per-profile callbacks.

#pragma once

#include <vd2/system/VDString.h>

enum class ATMetadataAccessMode : int {
	Disabled = 0,
	OnDemand = 1,
	Automatic = 2,
};

struct ATMetadataSettings {
	// Network consent. OnDemand is the privacy-preserving default: explicit
	// Get Metadata actions work, but library scans never contact a provider.
	ATMetadataAccessMode mAccessMode = ATMetadataAccessMode::OnDemand;
	// --- Account -----------------------------------------------------
	// When false the request is anonymous: quota is charged to the
	// application's developer credential and shared with every other
	// AltirraSDL user, so it is deliberately rate-limited hard.
	bool      mbUseUserAccount = false;
	VDStringA mUserName;
	VDStringA mUserPassword;

	// Advanced: a developer credential supplied by the user or by a
	// distribution packager.  Only consulted when the build has none
	// baked in (see ATMetadataHaveDevCredential).
	VDStringA mCustomDevId;
	VDStringA mCustomDevPassword;

	// --- What to fetch -----------------------------------------------
	bool mbDownloadText       = true;
	bool mbDownloadBoxArt     = true;
	bool mbDownloadTitleShot  = true;
	bool mbDownloadScreenshot = true;
	bool mbDownloadLogo       = false;

	// Index into kATMetadataArtSlotNames (and into GameArtSlot) — which
	// kind of downloaded image the library shows, everywhere: grid tiles,
	// list thumbnails and both details panels.
	//
	// This is a *live view preference*, not a per-game property and not a
	// scrape-time seed.  Changing it re-renders the whole library at once,
	// which is what makes the left/right art switch in the details panel
	// worth having.  Games missing the chosen kind fall back through
	// ATGameResolveArtSlot's fixed order rather than showing nothing.
	//
	// Default 2 = Screenshot: it is the one image that shows what the
	// game actually looks like, and Atari 8-bit box art coverage is far
	// patchier than screenshot coverage.
	int  mArtSlot = 2;

	// --- Preferences -------------------------------------------------
	VDStringA mRegion{"wor"};      // see kATMetadataRegionCodes
	VDStringA mLanguage{"en"};     // see kATMetadataLanguageCodes
	bool mbTry5200Fallback   = true;

	// Fuzzy name search for files that no hash and no exact name could
	// identify.  Worth having on an 8-bit library, which is mostly
	// cracked, trained and hand-renamed files rather than pristine
	// archive dumps — but it spends two extra requests per unmatched
	// game against a shared quota, and a fuzzy hit is a guess, so it is
	// a setting rather than unconditional.
	bool mbFuzzyNameMatch    = true;

	// Legacy mirror of mAccessMode == Automatic. Automatic access is
	// opt-in; the default OnDemand mode never starts network work merely
	// because games appeared in the library.
	//
	// Deliberately bounded rather than unconditional — see
	// kATMetadataAutoFetchMax.  A first import of several thousand
	// unmatched files is a decision the user should make, not something
	// that happens to them while they are looking at the grid.
	bool mbAutoFetchNewGames = false;

	// --- One-shot UI state -------------------------------------------
	bool mbFirstRunNudgeShown = false;
	bool mbConsentRecorded = false;
};

// Region / language / art-slot tables, shared by both frontends so the
// Desktop dialog and the Gaming Mode page can never drift apart.
extern const char *const kATMetadataRegionCodes[];
extern const char *const kATMetadataRegionNames[];
extern const int         kATMetadataRegionCount;

extern const char *const kATMetadataLanguageCodes[];
extern const char *const kATMetadataLanguageNames[];
extern const int         kATMetadataLanguageCount;

extern const char *const kATMetadataArtSlotNames[];
extern const int         kATMetadataArtSlotCount;

int ATMetadataFindRegionIndex(const char *code);
int ATMetadataFindLanguageIndex(const char *code);

// The live settings object.  Mutate through this, then call
// ATMetadataSaveSettings().
// Most newly-seen games an automatic fetch will take on in one go.
// Above this the library is being imported, not extended, and the user
// gets the explicit "Download what's missing" action instead — which
// matters because anonymous requests are charged to a credential shared
// with every other AltirraSDL user.
enum : int { kATMetadataAutoFetchMax = 25 };

ATMetadataSettings& ATMetadataGetSettings();

// Single policy check used by every ScreenScraper network boundary.
// OnDemand and Automatic permit requests; Disabled forbids them.
inline bool ATMetadataNetworkAllowed(const ATMetadataSettings& settings) {
	return settings.mAccessMode != ATMetadataAccessMode::Disabled;
}

void ATMetadataLoadSettings();

// Writes the registry and flushes it to disk immediately.  The flush is
// deliberate: on Android the process can be killed without a clean
// exit, and losing a just-typed password would be a bad surprise.
void ATMetadataSaveSettings();

// --- Developer credential --------------------------------------------
//
// Identifies AltirraSDL to the provider.  Baked in at configure time
// from the build environment (see tools/gen_ss_devkey.py); when absent,
// the user's Advanced override is used instead.
//
// A build with neither is normal and fully supported — the UI reports
// "not configured" and points at the Advanced field.

// True when a credential is available from either source.
bool ATMetadataHaveDevCredential();

// True when this build was compiled with a baked-in credential, so the
// UI can distinguish "the build ships one" from "you supplied one".
bool ATMetadataHaveBakedDevCredential();

// Fills the effective credential.  Returns false when there is none.
bool ATMetadataGetDevCredential(VDStringA& outDevId,
	VDStringA& outDevPassword);

// Developer debug password, read from ALTIRRA_SS_DEBUG_PASSWORD.  Only
// ever non-empty in a build configured with ALTIRRA_SS_DEBUG, and never
// stored, committed or transmitted to CI — it can force quota counters,
// spoof IPs and escalate account level.  Returns an empty string in
// every shipping build.
const char *ATMetadataGetDebugPassword();
