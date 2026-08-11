//	AltirraSDL - Game metadata scraper settings
//	See metadata_settings.h for the storage rationale.

#include <stdafx.h>

#include "metadata_settings.h"

#include <cstdlib>
#include <cstring>
#include <vd2/system/registry.h>

#include "screenscraper_devkey.h"

extern void ATRegistryFlushToDisk();

namespace {

const char kMetadataKey[] = "Metadata";

ATMetadataSettings g_settings;
bool g_loaded = false;

// -------------------------------------------------------------------------
// At-rest obfuscation for the stored account password.
//
// The provider has no token or OAuth flow: the plaintext password is a
// query-string parameter on every request, so the app must be able to
// reproduce it and therefore cannot hash it.  The XOR below keeps it
// from appearing in settings.ini as a readable string next to the
// username — it is obfuscation, not encryption, and the UI says so in
// as many words next to the field.
// -------------------------------------------------------------------------
const unsigned char kPwKey[] = {
	0x74, 0x1D, 0xC6, 0x3B, 0xA8, 0x52, 0xEF, 0x09,
	0x9B, 0x64, 0xD1, 0x27, 0xB3, 0x4E, 0x86, 0xFA,
};

VDStringA ObfuscateToHex(const VDStringA& plain) {
	static const char kHex[] = "0123456789ABCDEF";
	VDStringA out;
	out.reserve(plain.size() * 2);
	for (size_t i = 0; i < plain.size(); ++i) {
		const unsigned char b = (unsigned char)plain[i]
			^ kPwKey[i % sizeof kPwKey]
			^ (unsigned char)((i * 29 + 7) & 0xFF);
		out += kHex[b >> 4];
		out += kHex[b & 15];
	}
	return out;
}

int HexVal(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

VDStringA DeobfuscateFromHex(const VDStringA& hex) {
	VDStringA out;
	if (hex.size() & 1)
		return out;
	out.reserve(hex.size() / 2);
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		const int hi = HexVal(hex[i]);
		const int lo = HexVal(hex[i + 1]);
		if (hi < 0 || lo < 0)
			return VDStringA();
		const size_t n = i / 2;
		const unsigned char b = (unsigned char)((hi << 4) | lo)
			^ kPwKey[n % sizeof kPwKey]
			^ (unsigned char)((n * 29 + 7) & 0xFF);
		out += (char)b;
	}
	return out;
}

// -------------------------------------------------------------------------
// Baked-in developer credential, de-obfuscated on first use.
// -------------------------------------------------------------------------
[[maybe_unused]] VDStringA DeobfuscateDevField(const unsigned char *data,
	size_t len)
{
	VDStringA out;
	out.reserve(len);
	for (size_t i = 0; i < len; ++i) {
		const unsigned char b = data[i]
			^ kATSSObfKey[i % sizeof kATSSObfKey]
			^ (unsigned char)((i * 37 + 11) & 0xFF);
		out += (char)b;
	}
	return out;
}

VDStringA ReadString(VDRegistryKey& key, const char *name,
	const char *defaultValue)
{
	VDStringA value;
	if (!key.getString(name, value))
		value = defaultValue;
	return value;
}

}  // namespace

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------

// "wor" (world) first and default: it produces the most matches, and a
// missing title is a worse outcome than a slightly unfamiliar one.
const char *const kATMetadataRegionCodes[] = {
	"wor", "us", "eu", "uk", "de", "fr", "jp",
};
const char *const kATMetadataRegionNames[] = {
	"World", "USA", "Europe", "United Kingdom", "Germany", "France", "Japan",
};
const int kATMetadataRegionCount =
	(int)(sizeof kATMetadataRegionCodes / sizeof kATMetadataRegionCodes[0]);

const char *const kATMetadataLanguageCodes[] = {
	"en", "de", "es", "fr", "it", "pt",
};
const char *const kATMetadataLanguageNames[] = {
	"English", "German", "Spanish", "French", "Italian", "Portuguese",
};
const int kATMetadataLanguageCount =
	(int)(sizeof kATMetadataLanguageCodes / sizeof kATMetadataLanguageCodes[0]);

// Order must match enum GameArtSlot.
const char *const kATMetadataArtSlotNames[] = {
	"Box art", "Title screen", "Screenshot", "Logo",
};
const int kATMetadataArtSlotCount =
	(int)(sizeof kATMetadataArtSlotNames / sizeof kATMetadataArtSlotNames[0]);

int ATMetadataFindRegionIndex(const char *code) {
	if (code) {
		for (int i = 0; i < kATMetadataRegionCount; ++i) {
			if (strcmp(kATMetadataRegionCodes[i], code) == 0)
				return i;
		}
	}
	return 0;
}

int ATMetadataFindLanguageIndex(const char *code) {
	if (code) {
		for (int i = 0; i < kATMetadataLanguageCount; ++i) {
			if (strcmp(kATMetadataLanguageCodes[i], code) == 0)
				return i;
		}
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Load / save
// ---------------------------------------------------------------------------

ATMetadataSettings& ATMetadataGetSettings() {
	if (!g_loaded)
		ATMetadataLoadSettings();
	return g_settings;
}

void ATMetadataLoadSettings() {
	g_loaded = true;

	VDRegistryAppKey key(kMetadataKey, false);

	ATMetadataSettings s;
	const int consentVersion = key.getInt("ConsentVersion", 0);
	int accessMode = key.getInt("AccessMode",
		(int)ATMetadataAccessMode::OnDemand);
	if (accessMode < (int)ATMetadataAccessMode::Disabled
		|| accessMode > (int)ATMetadataAccessMode::Automatic)
		accessMode = (int)ATMetadataAccessMode::OnDemand;
	s.mAccessMode = (ATMetadataAccessMode)accessMode;
	s.mbConsentRecorded = consentVersion >= 1;

	s.mbUseUserAccount = key.getBool("UseUserAccount", s.mbUseUserAccount);
	s.mUserName = ReadString(key, "UserName", "");
	s.mUserPassword = DeobfuscateFromHex(ReadString(key, "UserPasswordObf", ""));

	s.mCustomDevId = ReadString(key, "CustomDevId", "");
	s.mCustomDevPassword =
		DeobfuscateFromHex(ReadString(key, "CustomDevPasswordObf", ""));

	s.mbDownloadText       = key.getBool("DownloadText", s.mbDownloadText);
	s.mbDownloadBoxArt     = key.getBool("DownloadBoxArt", s.mbDownloadBoxArt);
	s.mbDownloadTitleShot  = key.getBool("DownloadTitleShot", s.mbDownloadTitleShot);
	s.mbDownloadScreenshot = key.getBool("DownloadScreenshot", s.mbDownloadScreenshot);
	s.mbDownloadLogo       = key.getBool("DownloadLogo", s.mbDownloadLogo);

	// Deliberately a NEW key rather than a rename of "TileImageSlot".
	// That value meant "which slot to stamp on an entry as it is
	// scraped", which is a different question from "which slot do I want
	// to look at"; reusing it would silently reinterpret a stale answer,
	// and would also deny existing installs the new Screenshot default.
	s.mbFuzzyNameMatch = key.getBool("FuzzyNameMatch", s.mbFuzzyNameMatch);
	// AutoFetchNewGames used to default true without an explicit consent
	// step. Do not reinterpret that legacy value as consent on upgrade.
	// Once ConsentVersion is present, AccessMode is authoritative.
	s.mbAutoFetchNewGames = s.mbConsentRecorded
		&& s.mAccessMode == ATMetadataAccessMode::Automatic;
	s.mArtSlot = key.getInt("ArtSlot", s.mArtSlot);
	if (s.mArtSlot < 0 || s.mArtSlot >= kATMetadataArtSlotCount)
		s.mArtSlot = 2;

	s.mRegion = ReadString(key, "Region", s.mRegion.c_str());
	s.mLanguage = ReadString(key, "Language", s.mLanguage.c_str());
	// Guard against a hand-edited settings.ini carrying a code we would
	// then send verbatim to the provider.
	s.mRegion = kATMetadataRegionCodes[ATMetadataFindRegionIndex(s.mRegion.c_str())];
	s.mLanguage = kATMetadataLanguageCodes[ATMetadataFindLanguageIndex(s.mLanguage.c_str())];

	s.mbTry5200Fallback    = key.getBool("Try5200Fallback", s.mbTry5200Fallback);
	s.mbFirstRunNudgeShown = key.getBool("FirstRunNudgeShown", s.mbFirstRunNudgeShown);

	g_settings = std::move(s);
}

void ATMetadataSaveSettings() {
	VDRegistryAppKey key(kMetadataKey, true);
	const ATMetadataSettings& s = g_settings;
	key.setInt("ConsentVersion", s.mbConsentRecorded ? 1 : 0);
	key.setInt("AccessMode", (int)s.mAccessMode);

	key.setBool("UseUserAccount", s.mbUseUserAccount);
	key.setString("UserName", s.mUserName.c_str());
	key.setString("UserPasswordObf", ObfuscateToHex(s.mUserPassword).c_str());

	key.setString("CustomDevId", s.mCustomDevId.c_str());
	key.setString("CustomDevPasswordObf",
		ObfuscateToHex(s.mCustomDevPassword).c_str());

	key.setBool("DownloadText", s.mbDownloadText);
	key.setBool("DownloadBoxArt", s.mbDownloadBoxArt);
	key.setBool("DownloadTitleShot", s.mbDownloadTitleShot);
	key.setBool("DownloadScreenshot", s.mbDownloadScreenshot);
	key.setBool("DownloadLogo", s.mbDownloadLogo);

	key.setBool("FuzzyNameMatch", s.mbFuzzyNameMatch);
	key.setBool("AutoFetchNewGames",
		s.mbConsentRecorded
			&& s.mAccessMode == ATMetadataAccessMode::Automatic);
	key.setInt("ArtSlot", s.mArtSlot);

	key.setString("Region", s.mRegion.c_str());
	key.setString("Language", s.mLanguage.c_str());

	key.setBool("Try5200Fallback", s.mbTry5200Fallback);
	key.setBool("FirstRunNudgeShown", s.mbFirstRunNudgeShown);

	// Immediate flush — Android may kill the process without a clean
	// exit, and a just-typed credential must not be lost.
	ATRegistryFlushToDisk();
}

// ---------------------------------------------------------------------------
// Developer credential
// ---------------------------------------------------------------------------

bool ATMetadataHaveBakedDevCredential() {
#if ALTIRRA_SS_HAVE_DEVKEY
	return true;
#else
	return false;
#endif
}

bool ATMetadataGetDevCredential(VDStringA& outDevId,
	VDStringA& outDevPassword)
{
#if ALTIRRA_SS_HAVE_DEVKEY
	outDevId = DeobfuscateDevField(kATSSDevIdObf, sizeof kATSSDevIdObf);
	outDevPassword = DeobfuscateDevField(kATSSDevPwObf, sizeof kATSSDevPwObf);
	if (!outDevId.empty() && !outDevPassword.empty())
		return true;
#endif

	const ATMetadataSettings& s = ATMetadataGetSettings();
	if (!s.mCustomDevId.empty() && !s.mCustomDevPassword.empty()) {
		outDevId = s.mCustomDevId;
		outDevPassword = s.mCustomDevPassword;
		return true;
	}

	outDevId.clear();
	outDevPassword.clear();
	return false;
}

bool ATMetadataHaveDevCredential() {
	VDStringA id, pw;
	return ATMetadataGetDevCredential(id, pw);
}

const char *ATMetadataGetDebugPassword() {
#if defined(ALTIRRA_SS_DEBUG)
	const char *env = getenv("ALTIRRA_SS_DEBUG_PASSWORD");
	return env ? env : "";
#else
	return "";
#endif
}
