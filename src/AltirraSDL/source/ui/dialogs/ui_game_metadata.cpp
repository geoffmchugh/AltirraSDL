//	AltirraSDL - Game metadata UI
//	See ui_game_metadata.h for the split between shared logic and the
//	per-frontend chrome.

#include <SDL3/SDL.h>
#include <imgui.h>

#include <atomic>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

#include <vd2/system/VDString.h>
#include <vd2/system/text.h>

#include "ui_game_metadata.h"
#include "ui_confirm_dialog.h"
#include "ui/gamelibrary/game_library.h"
#include "ui/gamelibrary/game_library_art.h"
#include "media/metadata_scraper.h"
#include "media/metadata_screenscraper.h"
#include "media/metadata_settings.h"
#include "media/http_client.h"
#include "touch_widgets.h"
#include "ui_mode.h"

extern GameArtCache *GetGameArtCache();
extern void GameBrowser_Invalidate();

// ---------------------------------------------------------------------------
// Shared copy
// ---------------------------------------------------------------------------

const char *const kATMetadataProviderUrl = "https://www.screenscraper.fr/";

const char *const kATMetadataAccountHelpTitle = "How do I get an account?";

// Deliberately written as plain guidance rather than marketing: the user
// is being asked to create an account on a third-party site, so the
// reason has to be honest and the privacy consequence has to be stated.
const char *const kATMetadataAccountHelpBody =
	"1.  Open https://www.screenscraper.fr/\n"
	"2.  Click \"Inscription\" (top right) and register - it is free.\n"
	"3.  Confirm the e-mail they send you.\n"
	"4.  Type the same username and password here.\n"
	"\n"
	"Why bother?  Anonymous downloads share one small daily allowance "
	"with every other AltirraSDL user, so a large library will run out "
	"partway through.  A free account gives you your own allowance and "
	"lets several downloads run at once.  Contributing scans or box art "
	"to ScreenScraper raises it further.\n"
	"\n"
	"Your password is stored on this device so it can be sent with each "
	"request.  It is obfuscated, not encrypted - please don't reuse a "
	"password from another service.";

// ---------------------------------------------------------------------------
// Shared fact lines
//
// U+00B7 MIDDLE DOT, padded with two spaces either side.  It sits in the
// Latin-1 range every font in the atlas covers, and it reads as a
// separator rather than as punctuation belonging to either neighbour.
// ---------------------------------------------------------------------------

namespace {

const char kSep[] = "  \xC2\xB7  ";

void AppendFact(VDStringA& line, const char *text) {
	if (!text || !*text)
		return;
	if (!line.empty())
		line += kSep;
	line += text;
}

}  // namespace

VDStringA ATUIMetadataDisplayTitle(const GameEntry& entry) {
	VDStringA title = VDTextWToU8(entry.mMeta.mTitle);
	if (title.empty())
		title = VDTextWToU8(entry.mDisplayName);
	return title;
}

VDStringA ATUIMetadataFactsLine(const GameEntry& entry) {
	const GameMetadata& m = entry.mMeta;
	VDStringA line;
	char buf[32];

	AppendFact(line, VDTextWToU8(m.mPublisher).c_str());

	// Developer only when it is genuinely different information —
	// on most Atari 8-bit titles the two fields carry the same name and
	// printing both just looks like a rendering bug.
	if (!m.mDeveloper.empty() && m.mDeveloper != m.mPublisher)
		AppendFact(line, VDTextWToU8(m.mDeveloper).c_str());

	if (m.mYear) {
		snprintf(buf, sizeof buf, "%u", (unsigned)m.mYear);
		AppendFact(line, buf);
	}
	if (m.mRating) {
		snprintf(buf, sizeof buf, "%u/20", (unsigned)m.mRating);
		AppendFact(line, buf);
	}
	return line;
}

VDStringA ATUIMetadataGenreLine(const GameEntry& entry) {
	const GameMetadata& m = entry.mMeta;
	VDStringA line = VDTextWToU8(m.mGenre);
	if (m.mPlayersMax > 1) {
		char buf[32];
		snprintf(buf, sizeof buf, "up to %u players", (unsigned)m.mPlayersMax);
		AppendFact(line, buf);
	}
	return line;
}

VDStringA ATUIMetadataCopyLine(const GameEntry& entry) {
	VDStringA line;
	char buf[64];

	const char *typeName = nullptr;
	if (!entry.mVariants.empty()) {
		switch (entry.mVariants[0].mType) {
			case GameMediaType::Disk:       typeName = "Disk"; break;
			case GameMediaType::Executable: typeName = "Executable"; break;
			case GameMediaType::Cartridge:  typeName = "Cartridge"; break;
			case GameMediaType::Cassette:   typeName = "Cassette"; break;
			default: break;
		}
	}
	AppendFact(line, typeName);

	if (entry.mVariants.size() > 1) {
		snprintf(buf, sizeof buf, "%zu variants", entry.mVariants.size());
		AppendFact(line, buf);
	}

	if (entry.mPlayCount) {
		snprintf(buf, sizeof buf, "played %u time%s",
			(unsigned)entry.mPlayCount, entry.mPlayCount == 1 ? "" : "s");
		AppendFact(line, buf);
	}

	return line;
}

const char *ATUIMetadataEmptyReason(const GameEntry& entry) {
	switch (entry.mMeta.mStatus) {
		case GameMetaStatus::None:
			return "No metadata downloaded for this game yet.";
		case GameMetaStatus::NotFound:
			return "ScreenScraper has no entry for this file.";
		case GameMetaStatus::Error:
			return "The last download for this game did not finish.";
		default:
			return "";
	}
}

namespace {

// ---------------------------------------------------------------------------
// Account test worker
// ---------------------------------------------------------------------------

std::thread            g_testThread;
std::atomic<bool>      g_testCancel{false};
std::atomic<int>       g_testState{(int)ATMetadataTestState::Idle};
std::atomic<bool>      g_testFinished{false};
std::mutex             g_testMutex;
VDStringA              g_testMessage;

void SetTestMessage(const VDStringA& text) {
	std::lock_guard<std::mutex> lock(g_testMutex);
	g_testMessage = text;
}

// ---------------------------------------------------------------------------
// Media storage helpers
// ---------------------------------------------------------------------------

VDStringA MediaDir(const ATGameLibrary& lib) {
	VDStringA dir = lib.GetConfigDir();
	if (!dir.empty() && dir.back() != '/')
		dir += '/';
	dir += "media";
	return dir;
}

// Delete every media file an entry owns.  Best-effort: a file that is
// already gone is not an error.
void DeleteEntryMedia(const ATGameLibrary& lib, const GameEntry& entry,
	GameArtCache *artCache)
{
	const VDStringW *slots[4] = {
		&entry.mMeta.mBoxArtPath, &entry.mMeta.mTitleShotPath,
		&entry.mMeta.mScreenshotPath, &entry.mMeta.mLogoPath,
	};

	for (const VDStringW *slot : slots) {
		if (slot->empty())
			continue;
		const VDStringW absolute = lib.ResolveMediaPath(*slot);
		if (absolute.empty())
			continue;
		// Drop the scaled thumbnail before the source, otherwise the
		// stale thumbnail outlives the file it was made from.
		if (artCache)
			artCache->Invalidate(absolute);
		SDL_RemovePath(VDTextWToU8(absolute).c_str());
	}
}

// ---------------------------------------------------------------------------
// Desktop tab state
// ---------------------------------------------------------------------------

char g_userNameBuf[128] = {};
char g_passwordBuf[128] = {};
char g_devIdBuf[128] = {};
char g_devPasswordBuf[128] = {};
bool g_credBufsSeeded = false;
bool g_showPassword = false;
bool g_showAdvanced = false;
bool g_helpOpen = false;
bool g_helpAutoOpened = false;

// Remove-confirmation state.  The path is the identity; the index is
// only a cache of where that path was last seen.
bool      g_openRemove = false;
int       g_removeEntry = -1;
VDStringA g_removeName;
VDStringW g_removePath;

void SeedCredentialBuffers() {
	if (g_credBufsSeeded)
		return;
	g_credBufsSeeded = true;
	const ATMetadataSettings& s = ATMetadataGetSettings();
	snprintf(g_userNameBuf, sizeof g_userNameBuf, "%s", s.mUserName.c_str());
	snprintf(g_passwordBuf, sizeof g_passwordBuf, "%s", s.mUserPassword.c_str());
	snprintf(g_devIdBuf, sizeof g_devIdBuf, "%s", s.mCustomDevId.c_str());
	snprintf(g_devPasswordBuf, sizeof g_devPasswordBuf, "%s",
		s.mCustomDevPassword.c_str());
}

void CommitCredentials() {
	ATMetadataSettings& s = ATMetadataGetSettings();
	s.mUserName = g_userNameBuf;
	s.mUserPassword = g_passwordBuf;
	s.mCustomDevId = g_devIdBuf;
	s.mCustomDevPassword = g_devPasswordBuf;
	ATMetadataSaveSettings();
}

VDStringA FormatBytes(uint64_t bytes) {
	char buf[64];
	if (bytes >= 1024ull * 1024ull * 1024ull) {
		snprintf(buf, sizeof buf, "%.1f GB",
			(double)bytes / (1024.0 * 1024.0 * 1024.0));
	} else if (bytes >= 1024ull * 1024ull) {
		snprintf(buf, sizeof buf, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
	} else if (bytes >= 1024ull) {
		snprintf(buf, sizeof buf, "%.1f KB", (double)bytes / 1024.0);
	} else {
		snprintf(buf, sizeof buf, "%u bytes", (unsigned)bytes);
	}
	return VDStringA(buf);
}

}  // namespace

// ---------------------------------------------------------------------------
// Account test
// ---------------------------------------------------------------------------

void ATUIMetadataStartAccountTest() {
	if (g_testState.load() == (int)ATMetadataTestState::Running)
		return;

	if (g_testThread.joinable())
		g_testThread.join();

	g_testCancel.store(false);
	g_testFinished.store(false);
	g_testState.store((int)ATMetadataTestState::Running);
	SetTestMessage(VDStringA("Contacting ScreenScraper..."));

	// The settings are copied here, on the main thread, so the worker
	// never reads a struct the UI may be editing under it.
	const ATMetadataSettings settings = ATMetadataGetSettings();

	g_testThread = std::thread([settings]() {
		ATScreenScraperAccount account;
		const bool ok = ATScreenScraperFetchAccount(settings, &g_testCancel,
			account);

		char buf[256];
		if (ok && account.mbValid) {
			if (account.mMaxRequestsPerDay > 0) {
				snprintf(buf, sizeof buf,
					"Level %s  -  %d thread%s  -  %d / %d requests today",
					account.mLevel.empty() ? "?" : account.mLevel.c_str(),
					account.mMaxThreads, account.mMaxThreads == 1 ? "" : "s",
					account.mRequestsToday, account.mMaxRequestsPerDay);
			} else {
				snprintf(buf, sizeof buf, "Signed in  -  %d thread%s",
					account.mMaxThreads, account.mMaxThreads == 1 ? "" : "s");
			}
			SetTestMessage(VDStringA(buf));
			g_testState.store((int)ATMetadataTestState::Success);
		} else {
			SetTestMessage(account.mError.empty()
				? VDStringA("Could not reach ScreenScraper.")
				: account.mError);
			g_testState.store((int)ATMetadataTestState::Failure);
		}
		g_testFinished.store(true);
	});
}

ATMetadataTestState ATUIMetadataGetTestState() {
	return (ATMetadataTestState)g_testState.load();
}

VDStringA ATUIMetadataGetTestMessage() {
	std::lock_guard<std::mutex> lock(g_testMutex);
	return g_testMessage;
}

void ATUIMetadataPumpAccountTest() {
	if (g_testFinished.exchange(false)) {
		if (g_testThread.joinable())
			g_testThread.join();
	}
}

void ATUIMetadataShutdownAccountTest() {
	g_testCancel.store(true);
	if (g_testThread.joinable())
		g_testThread.join();
	g_testState.store((int)ATMetadataTestState::Idle);
}

// ---------------------------------------------------------------------------
// Shared actions
// ---------------------------------------------------------------------------

bool ATUIMetadataIsUsable(VDStringA& outWhyNot) {
	if (!ATHttp::Available()) {
		outWhyNot = ATScreenScraperOutcomeText(
			ATScreenScraperOutcome::Unavailable);
		return false;
	}
	if (!ATMetadataHaveDevCredential()) {
		outWhyNot = ATScreenScraperOutcomeText(
			ATScreenScraperOutcome::NotConfigured);
		return false;
	}
	outWhyNot.clear();
	return true;
}

bool ATUIMetadataCanDownloadNow(VDStringA& outWhyNot) {
	if (!ATUIMetadataIsUsable(outWhyNot))
		return false;
	if (ATMetadataGetScraper().IsRunning()) {
		outWhyNot = "A metadata download is already running.";
		return false;
	}
	return true;
}

void ATUIMetadataStartDownload(ATGameLibrary& lib, bool onlyMissing) {
	ATMetadataScraper& scraper = ATMetadataGetScraper();
	scraper.ClearBanner();
	// Drop the previous run's verdict the instant a new one starts, so a
	// stale "no match" cannot sit next to a running progress bar.
	ATUIMetadataClearLastRun();
	scraper.Start(lib, ATMetadataSelectEntries(lib, onlyMissing));
}

void ATUIMetadataStartDownloadForEntry(ATGameLibrary& lib, int entryIndex) {
	if (entryIndex < 0 || (size_t)entryIndex >= lib.GetEntries().size())
		return;
	ATMetadataScraper& scraper = ATMetadataGetScraper();
	scraper.ClearBanner();
	ATUIMetadataClearLastRun();
	scraper.Start(lib, std::vector<int>{entryIndex});
}

void ATUIMetadataClearEntry(ATGameLibrary& lib, int entryIndex) {
	auto& entries = lib.GetEntries();
	if (entryIndex < 0 || (size_t)entryIndex >= entries.size())
		return;

	DeleteEntryMedia(lib, entries[entryIndex], GetGameArtCache());
	entries[entryIndex].mMeta = GameMetadata();
	lib.SaveCache();
	GameBrowser_Invalidate();
}

void ATUIMetadataClearAll(ATGameLibrary& lib) {
	GameArtCache *artCache = GetGameArtCache();
	auto& entries = lib.GetEntries();
	for (auto& e : entries) {
		DeleteEntryMedia(lib, e, artCache);
		e.mMeta = GameMetadata();
	}
	lib.SaveCache();
	GameBrowser_Invalidate();
}

void ATUIMetadataDeleteAllMedia(ATGameLibrary& lib) {
	GameArtCache *artCache = GetGameArtCache();
	auto& entries = lib.GetEntries();
	for (auto& e : entries) {
		DeleteEntryMedia(lib, e, artCache);
		e.mMeta.mBoxArtPath.clear();
		e.mMeta.mTitleShotPath.clear();
		e.mMeta.mScreenshotPath.clear();
		e.mMeta.mLogoPath.clear();
		// Text metadata is kept, but an entry that now has neither text
		// nor media is effectively unfetched.
		if (!e.mMeta.HasAnyText()
			&& e.mMeta.mStatus == GameMetaStatus::Matched)
		{
			e.mMeta.mStatus = GameMetaStatus::None;
		}
	}
	lib.SaveCache();
	GameBrowser_Invalidate();
}

void ATUIMetadataGetStorageUsage(const ATGameLibrary& lib,
	uint64_t& outBytes, int& outFiles)
{
	outBytes = 0;
	outFiles = 0;

	const VDStringA dir = MediaDir(lib);
	int count = 0;
	char **names = SDL_GlobDirectory(dir.c_str(), nullptr, 0, &count);
	if (!names)
		return;

	for (int i = 0; i < count; ++i) {
		VDStringA path = dir;
		path += '/';
		path += names[i];

		SDL_PathInfo info;
		if (SDL_GetPathInfo(path.c_str(), &info)
			&& info.type == SDL_PATHTYPE_FILE)
		{
			outBytes += (uint64_t)info.size;
			++outFiles;
		}
	}
	SDL_free(names);
}

namespace {
VDStringA g_lastRunText;
int       g_lastRunKind = 0;
}  // namespace

void ATUIMetadataPumpRunFeedback() {
	VDStringA text;
	ATMetadataScraper::RunReport kind = ATMetadataScraper::RunReport::None;
	if (!ATMetadataGetScraper().ConsumeRunReport(text, kind))
		return;
	if (text.empty())
		return;

	g_lastRunText = text;
	switch (kind) {
		case ATMetadataScraper::RunReport::Success: g_lastRunKind = 1; break;
		case ATMetadataScraper::RunReport::Warning: g_lastRunKind = 2; break;
		case ATMetadataScraper::RunReport::Failure: g_lastRunKind = 3; break;
		default:                                    g_lastRunKind = 0; break;
	}

	// Gaming Mode has a toast surface and no always-visible status strip,
	// so it gets the transient version.  A failure is held longer than a
	// success: it asks the user to decide something.
	if (ATUIIsGamingMode()) {
		ATTouchToastSeverity sev = ATTouchToastSeverity::Info;
		uint64_t ms = 4000;
		if (kind == ATMetadataScraper::RunReport::Success)
			sev = ATTouchToastSeverity::Success;
		else if (kind == ATMetadataScraper::RunReport::Warning)
			{ sev = ATTouchToastSeverity::Warning; ms = 7000; }
		else if (kind == ATMetadataScraper::RunReport::Failure)
			{ sev = ATTouchToastSeverity::Danger;  ms = 9000; }
		ATTouchPushToast(g_lastRunText.c_str(), sev, ms);
	}
}

void ATUIMetadataPumpResults(ATGameLibrary& lib) {
	GameArtCache *cache = GetGameArtCache();
	if (cache)
		cache->ProcessPending();
	if (ATMetadataGetScraper().ConsumeResults(lib, cache))
		GameBrowser_Invalidate();
}

void ATUIMetadataPumpAutoFetch(ATGameLibrary& lib) {
	auto& entries = lib.GetEntries();

	// Cheap early-out for the overwhelming majority of frames.
	bool anyPending = false;
	for (const auto& e : entries) {
		if (e.mbNewlyAdded) { anyPending = true; break; }
	}
	if (!anyPending)
		return;

	// --- Transient blockers: leave the flags alone and retry ---------
	//
	// This ordering is load-bearing.  AddBootedGame inserts the entry and
	// immediately calls StartScan(), so on the very frame a booted game's
	// flag appears there is ALWAYS a scan running.  Consuming the flags
	// before this check silently dropped every automatically-added game —
	// the feature looked switched off.
	//
	// A scan is also a genuine hazard, not just bad timing: it replaces
	// mEntries wholesale, taking the indices we are about to collect
	// with it.
	if (lib.IsScanning())
		return;
	// One run at a time.  Start() would refuse anyway, and refusing after
	// consuming the flags would lose the games.
	if (ATMetadataGetScraper().IsRunning())
		return;

	// --- Now commit: whatever we decide below is final for these -----
	std::vector<int> fresh;
	for (size_t i = 0; i < entries.size(); ++i) {
		if (!entries[i].mbNewlyAdded)
			continue;
		entries[i].mbNewlyAdded = false;
		if (entries[i].mMeta.mStatus == GameMetaStatus::None
			&& !entries[i].mVariants.empty())
		{
			fresh.push_back((int)i);
		}
	}

	if (fresh.empty())
		return;
	if (!ATMetadataGetSettings().mbAutoFetchNewGames)
		return;

	VDStringA whyNot;
	if (!ATUIMetadataCanDownloadNow(whyNot))
		return;

	// Too many to be an addition: this is an import, and it belongs to
	// the user, not to us.  The first-run nudge and the explicit
	// "Download what's missing" action both still offer it.
	if ((int)fresh.size() > kATMetadataAutoFetchMax)
		return;

	ATMetadataGetScraper().ClearBanner();
	ATUIMetadataClearLastRun();
	ATMetadataGetScraper().Start(lib, std::move(fresh));
}

const VDStringA& ATUIMetadataGetLastRunText() { return g_lastRunText; }
int  ATUIMetadataGetLastRunKind()             { return g_lastRunKind; }
void ATUIMetadataClearLastRun() {
	g_lastRunText.clear();
	g_lastRunKind = 0;
}

const char *ATUIMetadataStatusGlyph(GameMetaStatus status) {
	switch (status) {
		case GameMetaStatus::Matched:    return "OK";
		case GameMetaStatus::NotFound:   return "--";
		case GameMetaStatus::Error:      return "!";
		case GameMetaStatus::UserEdited: return "*";
		default:                         return "";
	}
}

const char *ATUIMetadataStatusLabel(GameMetaStatus status) {
	switch (status) {
		case GameMetaStatus::Matched:    return "Downloaded";
		case GameMetaStatus::NotFound:   return "Not found";
		case GameMetaStatus::Error:      return "Failed";
		case GameMetaStatus::UserEdited: return "Yes";
		default:                         return "Not downloaded";
	}
}

// ---------------------------------------------------------------------------
// Desktop: Games-tab toolbar
// ---------------------------------------------------------------------------

void ATUIRenderMetadataToolbar(ATGameLibrary& lib, int selectedEntry) {
	ATMetadataScraper& scraper = ATMetadataGetScraper();

	VDStringA whyNot;
	const bool usable = ATUIMetadataIsUsable(whyNot);
	const bool running = scraper.IsRunning();
	const bool scanning = lib.IsScanning();

	// While a run is in flight the primary action becomes Cancel, so the
	// user never has to hunt for it.
	if (running) {
		if (ImGui::Button("Cancel Download", ImVec2(180, 0)))
			scraper.Cancel();
	} else {
		const bool blocked = !usable || scanning || lib.GetEntryCount() == 0;
		if (blocked)
			ImGui::BeginDisabled();

		// Primary action is the cheap one: only what's missing.  The
		// expensive variants live in the dropdown.
		if (ImGui::Button("Download Metadata", ImVec2(180, 0)))
			ATUIMetadataStartDownload(lib, true);

		if (blocked)
			ImGui::EndDisabled();

		// Tooltips explain *why* a disabled button is disabled — a
		// greyed-out control with no explanation is a dead end.
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			if (!usable)
				ImGui::SetTooltip("%s", whyNot.c_str());
			else if (scanning)
				ImGui::SetTooltip("Wait for the library scan to finish.");
			else if (lib.GetEntryCount() == 0)
				ImGui::SetTooltip("Add a source folder first.");
			else
				ImGui::SetTooltip("Download metadata for games that "
					"don't have any yet.");
		}

		ImGui::SameLine(0, 2);
		if (blocked)
			ImGui::BeginDisabled();
		if (ImGui::Button("v##dlmenu", ImVec2(24, 0)))
			ImGui::OpenPopup("##dlmenu_popup");
		if (blocked)
			ImGui::EndDisabled();

		if (ImGui::BeginPopup("##dlmenu_popup")) {
			char label[96];
			snprintf(label, sizeof label, "Download for missing only (%d)",
				ATMetadataCountEntries(lib, true));
			if (ImGui::MenuItem(label))
				ATUIMetadataStartDownload(lib, true);

			snprintf(label, sizeof label, "Re-download for all games (%d)",
				ATMetadataCountEntries(lib, false));
			if (ImGui::MenuItem(label))
				ATUIMetadataStartDownload(lib, false);

			ImGui::Separator();

			const bool haveSelection = selectedEntry >= 0
				&& (size_t)selectedEntry < lib.GetEntries().size();
			if (ImGui::MenuItem("Download for selected game", nullptr, false,
				haveSelection))
			{
				ATUIMetadataStartDownloadForEntry(lib, selectedEntry);
			}
			ImGui::EndPopup();
		}
	}

	// Counts.  Cheap enough to recompute per frame at library sizes we
	// care about, and always correct.
	int matched = 0, notFound = 0, none = 0;
	for (const auto& e : lib.GetEntries()) {
		switch (e.mMeta.mStatus) {
			case GameMetaStatus::Matched:
			case GameMetaStatus::UserEdited: ++matched; break;
			case GameMetaStatus::NotFound:   ++notFound; break;
			default:                          ++none; break;
		}
	}

	ImGui::SameLine();
	ImGui::TextDisabled("%d with metadata  -  %d not found  -  %d not tried",
		matched, notFound, none);
}

// ---------------------------------------------------------------------------
// Desktop: per-row context menu
// ---------------------------------------------------------------------------

void ATUIRenderMetadataRowMenu(ATGameLibrary& lib, int entryIndex) {
	auto& entries = lib.GetEntries();
	if (entryIndex < 0 || (size_t)entryIndex >= entries.size())
		return;

	const GameEntry& e = entries[entryIndex];

	VDStringA whyNot;
	const bool usable = ATUIMetadataCanDownloadNow(whyNot);

	if (ImGui::MenuItem("Download metadata", nullptr, false, usable))
		ATUIMetadataStartDownloadForEntry(lib, entryIndex);
	// A context-menu item cannot carry a tooltip, so say it inline.
	if (!usable && !whyNot.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text,
			ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("%s", whyNot.c_str());
		ImGui::PopStyleColor();
	}

	if (ImGui::MenuItem("Clear metadata", nullptr, false,
		e.mMeta.mStatus != GameMetaStatus::None))
	{
		ATUIMetadataClearEntry(lib, entryIndex);
	}

	ImGui::Separator();

	// Destructive-looking, so it sits below a separator and behind a
	// confirmation.  It removes the library entry, never the file.
	if (ImGui::MenuItem("Remove from library..."))
		ATUIMetadataOpenRemove(lib, entryIndex);
}

// ---------------------------------------------------------------------------
// Desktop: progress strip
// ---------------------------------------------------------------------------

bool ATUIRenderMetadataProgress(ATGameLibrary& lib) {
	ATMetadataScraper& scraper = ATMetadataGetScraper();
	const VDStringA banner = scraper.GetBanner();
	const bool running = scraper.IsRunning();
	// The end-of-run message stays on screen until dismissed.  Desktop
	// has the room for it, and a result the user has to catch within a
	// few seconds is a result they will miss.
	const VDStringA& lastRun = ATUIMetadataGetLastRunText();

	if (!running && banner.empty() && lastRun.empty())
		return false;

	ImGui::Separator();

	if (running) {
		const int total = scraper.GetTotal();
		const int done = scraper.GetDone();
		const float fraction = total > 0 ? (float)done / (float)total : 0.0f;

		char overlay[64];
		snprintf(overlay, sizeof overlay, "%d / %d", done, total);
		ImGui::ProgressBar(fraction, ImVec2(-1, 0), overlay);

		const VDStringA current = scraper.GetCurrentName();
		ImGui::TextDisabled("%s", current.empty() ? "Working..." : current.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("   %d found  -  %d not found  -  %d failed",
			scraper.GetMatched(), scraper.GetNotFound(), scraper.GetErrors());

		ImGui::SameLine();
		const float buttonW = 90.0f;
		const float avail = ImGui::GetContentRegionMax().x;
		ImGui::SetCursorPosX(avail - buttonW);
		if (ImGui::Button("Cancel", ImVec2(buttonW, 0)))
			scraper.Cancel();
	}

	// A finished run's outcome.  Suppressed while a new run is already
	// under way, and suppressed when it merely repeats the banner.
	if (!running && !lastRun.empty() && lastRun != banner) {
		const int kind = ATUIMetadataGetLastRunKind();
		ImVec4 colour(0.55f, 0.80f, 0.55f, 1.0f);          // ok
		if (kind == 2) colour = ImVec4(1.0f, 0.80f, 0.40f, 1.0f);
		if (kind == 3) colour = ImVec4(1.0f, 0.50f, 0.45f, 1.0f);
		ImGui::PushStyleColor(ImGuiCol_Text, colour);
		ImGui::TextWrapped("%s", lastRun.c_str());
		ImGui::PopStyleColor();
		if (ImGui::SmallButton("OK##lastrun"))
			ATUIMetadataClearLastRun();
	}

	if (!banner.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.35f, 1.0f));
		ImGui::TextWrapped("%s", banner.c_str());
		ImGui::PopStyleColor();

		if (!running) {
			if (ImGui::SmallButton("Dismiss"))
				scraper.ClearBanner();
			// The most common banner is a quota problem, and the fix is
			// always the same, so offer it right there.
			ImGui::SameLine();
			if (ImGui::SmallButton("Open Metadata settings")) {
				scraper.ClearBanner();
				g_helpOpen = true;
			}
		}
	}

	(void)lib;
	return true;
}

// ---------------------------------------------------------------------------
// Desktop: details pane
// ---------------------------------------------------------------------------

namespace {

// Scale (down only) to fit inside maxW x maxH, preserving aspect.
ImVec2 FitInside(int w, int h, float maxW, float maxH) {
	if (w <= 0 || h <= 0)
		return ImVec2(0, 0);
	float scale = maxW / (float)w;
	const float scaleH = maxH / (float)h;
	if (scaleH < scale)
		scale = scaleH;
	if (scale > 1.0f)
		scale = 1.0f;   // never upscale a small cover into a blurry slab
	return ImVec2((float)w * scale, (float)h * scale);
}

// The hero image: whatever GetTileArtPath resolves to, letterboxed into
// a fixed maxW x maxH box.
//
// Fixed, not shrink-to-fit, and that is the whole point.  Box art is
// portrait, screenshots are landscape and logos are wide, so sizing the
// box to the image means the entire pane below it jumps every time the
// selection moves to a game with differently-shaped art — or, worse,
// every time the user picks a different tile image from the strip
// directly underneath, which then slides out from under the cursor
// mid-click.  A constant box costs a little empty space and buys a
// layout that never moves.
//
// Handles the no-art and still-loading cases itself, so the caller never
// has to reserve space or draw a fallback.
void DrawHeroArt(ATGameLibrary& lib, const GameEntry& entry,
	float maxW, float maxH)
{
	GameArtCache *cache = GetGameArtCache();
	const VDStringW absolute = cache ? lib.GetTileArtPath(entry) : VDStringW();

	int w = 0, h = 0;
	ImTextureID tex = (cache && !absolute.empty())
		? cache->GetTexture(absolute, &w, &h) : (ImTextureID)0;

	const ImVec2 tl = ImGui::GetCursorScreenPos();
	const ImVec2 br(tl.x + maxW, tl.y + maxH);
	ImDrawList *dl = ImGui::GetWindowDrawList();

	if (tex && w > 0 && h > 0) {
		// A dark mat gives letterboxed art a defined edge instead of
		// bleeding into the pane background.
		dl->AddRectFilled(tl, br, IM_COL32(18, 18, 22, 255), 4.0f);
		const ImVec2 size = FitInside(w, h, maxW, maxH);
		const ImVec2 imgTL(tl.x + (maxW - size.x) * 0.5f,
			tl.y + (maxH - size.y) * 0.5f);
		dl->AddImage(tex, imgTL,
			ImVec2(imgTL.x + size.x, imgTL.y + size.y));
	} else {
		dl->AddRectFilled(tl, br,
			ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);

		// Distinguish "no art for this game" from "art is on its way":
		// a permanent state and a transient one should not look alike.
		const char *badge = "\xE2\x80\xA6";   // U+2026 HORIZONTAL ELLIPSIS
		if (absolute.empty()) {
			badge = "NO ART";
			if (!entry.mVariants.empty()) {
				switch (entry.mVariants[0].mType) {
				case GameMediaType::Disk:       badge = "DISK"; break;
				case GameMediaType::Executable: badge = "XEX";  break;
				case GameMediaType::Cartridge:  badge = "CART"; break;
				case GameMediaType::Cassette:   badge = "TAPE"; break;
				default: break;
				}
			}
		}
		const ImVec2 ts = ImGui::CalcTextSize(badge);
		dl->AddText(ImVec2(tl.x + (maxW - ts.x) * 0.5f,
			tl.y + (maxH - ts.y) * 0.5f),
			ImGui::GetColorU32(ImGuiCol_TextDisabled), badge);
	}

	ImGui::Dummy(ImVec2(maxW, maxH));
}

// Muted text that wraps.  ImGui has TextDisabled and TextWrapped but no
// combination of the two, and every fact line in the pane needs both:
// the pane is narrow by design, and an unwrapped line silently widens
// the scrolling child until the whole pane grows a horizontal scrollbar.
void TextDisabledWrapped(const char *text) {
	ImGui::PushStyleColor(ImGuiCol_Text,
		ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", text);
	ImGui::PopStyleColor();
}

}  // namespace

// ---------------------------------------------------------------------------
// Global artwork preference
// ---------------------------------------------------------------------------

void ATUIMetadataCycleArtSlot(int delta) {
	ATMetadataSettings& s = ATMetadataGetSettings();
	const int n = kATMetadataArtSlotCount;
	s.mArtSlot = ((s.mArtSlot + delta) % n + n) % n;
	ATMetadataSaveSettings();
	// Every tile, row and panel in the library is now showing the wrong
	// picture until the browser rebuilds.
	GameBrowser_Invalidate();
}

const char *ATUIMetadataArtSlotName() {
	int slot = ATMetadataGetSettings().mArtSlot;
	if (slot < 0 || slot >= kATMetadataArtSlotCount)
		slot = 0;
	return kATMetadataArtSlotNames[slot];
}

VDStringA ATUIMetadataArtCaption(const GameEntry& entry) {
	const GameArtSlot preferred = ATGameGetPreferredArtSlot();
	VDStringA caption(kATMetadataArtSlotNames[(int)preferred]);

	GameArtSlot shown;
	if (!ATGameResolveArtSlot(entry, preferred, shown))
		return caption;   // no media at all; the art box says so itself
	if (shown != preferred) {
		caption += "  \xC2\xB7  using ";
		caption += kATMetadataArtSlotNames[(int)shown];
	}
	return caption;
}

// ---------------------------------------------------------------------------
// Desktop: details pane
// ---------------------------------------------------------------------------

void ATUIRenderMetadataDetails(ATGameLibrary& lib, int entryIndex,
	void (*onLaunch)(void *userData, int entryIndex), void *userData)
{
	auto& entries = lib.GetEntries();
	if (entryIndex < 0 || (size_t)entryIndex >= entries.size()) {
		// Vertically centred so an empty pane reads as "nothing selected"
		// rather than as a rendering glitch in the top-left corner.
		const char *msg = "Select a game to see its details.";
		const ImVec2 avail = ImGui::GetContentRegionAvail();
		const ImVec2 ts = ImGui::CalcTextSize(msg);
		ImGui::SetCursorPos(ImVec2(
			ImGui::GetCursorPosX() + (avail.x - ts.x) * 0.5f,
			ImGui::GetCursorPosY() + (avail.y - ts.y) * 0.5f));
		ImGui::TextDisabled("%s", msg);
		return;
	}

	GameEntry& e = entries[entryIndex];
	const GameMetadata& m = e.mMeta;

	VDStringA whyNot;
	const bool canDownload = ATUIMetadataCanDownloadNow(whyNot);
	const bool canLaunch = !e.mVariants.empty();

	// Footer height: Play on its own row (it is the reason the pane
	// exists), then the metadata actions.  Reserved up front so the
	// scrolling body knows how much room it has.
	const ImGuiStyle& style = ImGui::GetStyle();
	const float footerH = ImGui::GetFrameHeightWithSpacing() * 2.0f
		+ style.ItemSpacing.y + 1.0f;

	// --- Scrolling body ---------------------------------------------
	// Everything except the action row scrolls, so a long synopsis can
	// never push Play out of reach.
	if (ImGui::BeginChild("##detailsScroll", ImVec2(0, -footerH))) {
		const float availW = ImGui::GetContentRegionAvail().x;
		// Proportional to the pane width, but clamped: the point is that
		// it depends only on the width, never on which image is showing,
		// so the layout still cannot shift as the selection moves.
		float heroH = availW * 0.75f;
		if (heroH < 160.0f) heroH = 160.0f;
		if (heroH > 240.0f) heroH = 240.0f;
		DrawHeroArt(lib, e, availW, heroH);

		// Artwork switch: [<]  Screenshot  [>]
		// Centred under the art it controls, so the association is
		// obvious without a label.
		{
			static const char *const kArtSwitchTip =
				"Choose which artwork the whole library shows.\n"
				"Games without that kind fall back to what they do have.";

			// The arrows are pinned to the pane's edges and the caption
			// is drawn centred between them.
			//
			// Centring the whole row instead would move both arrows every
			// time the caption's width changed — and the caption changes
			// on every press ("Screenshot" -> "Logo") and on every
			// selection change (the "· using Box art" fallback note).
			// The control would slide out from under the cursor mid-click,
			// which is the same defect the fixed art box and the fixed
			// thumbnail grid exist to avoid.
			const float rowX = ImGui::GetCursorPosX();
			const ImVec2 rowTL = ImGui::GetCursorScreenPos();
			const float arrowW = ImGui::GetFrameHeight();
			const float rowH = arrowW;

			if (ImGui::ArrowButton("##artprev", ImGuiDir_Left))
				ATUIMetadataCycleArtSlot(-1);
			// The tooltip belongs to each arrow, not to "whatever the
			// last item was" — hanging it off the end of the block would
			// silently attach it to the right arrow alone.
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", kArtSwitchTip);

			ImGui::SameLine(0, 0);
			ImGui::SetCursorPosX(rowX + availW - arrowW);
			if (ImGui::ArrowButton("##artnext", ImGuiDir_Right))
				ATUIMetadataCycleArtSlot(1);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", kArtSwitchTip);

			const VDStringA caption = ATUIMetadataArtCaption(e);
			const ImVec2 capSize = ImGui::CalcTextSize(caption.c_str());
			ImDrawList *dl = ImGui::GetWindowDrawList();
			dl->PushClipRect(ImVec2(rowTL.x + arrowW, rowTL.y),
				ImVec2(rowTL.x + availW - arrowW, rowTL.y + rowH), true);
			dl->AddText(ImVec2(rowTL.x + (availW - capSize.x) * 0.5f,
				rowTL.y + (rowH - capSize.y) * 0.5f),
				ImGui::GetColorU32(ImGuiCol_TextDisabled), caption.c_str());
			dl->PopClipRect();
		}
		ImGui::Spacing();

		// Title, then the scanned file name underneath when the provider
		// gave us a different one.  Showing both is what lets the user
		// confirm the match is correct rather than merely plausible.
		const VDStringA title = ATUIMetadataDisplayTitle(e);
		const VDStringA displayName = VDTextWToU8(e.mDisplayName);
		ImGui::PushStyleColor(ImGuiCol_Text,
			ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
		ImGui::TextWrapped("%s", title.c_str());
		ImGui::PopStyleColor();
		if (title != displayName)
			TextDisabledWrapped(displayName.c_str());

		const VDStringA facts = ATUIMetadataFactsLine(e);
		if (!facts.empty())
			TextDisabledWrapped(facts.c_str());

		const VDStringA genre = ATUIMetadataGenreLine(e);
		if (!genre.empty())
			TextDisabledWrapped(genre.c_str());

		if (!m.mDescription.empty()) {
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
			ImGui::TextWrapped("%s", VDTextWToU8(m.mDescription).c_str());
		}

		const char *emptyReason = ATUIMetadataEmptyReason(e);
		if (*emptyReason) {
			ImGui::Spacing();
			ImGui::TextWrapped("%s", emptyReason);
			if (!canDownload && !whyNot.empty())
				TextDisabledWrapped(whyNot.c_str());
		}

		// --- This copy ------------------------------------------------
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		const VDStringA copyLine = ATUIMetadataCopyLine(e);
		if (!copyLine.empty())
			TextDisabledWrapped(copyLine.c_str());

		if (e.mLastPlayed > 0) {
			char buf[64];
			const time_t tt = (time_t)e.mLastPlayed;
			struct tm tmv;
#ifdef _WIN32
			localtime_s(&tmv, &tt);
#else
			localtime_r(&tt, &tmv);
#endif
			strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tmv);
			ImGui::TextDisabled("last played %s", buf);
		}

		// The path is the one fact that tells the user *which* file this
		// row stands for, so it belongs here — wrapped, because library
		// paths are routinely wider than the pane.
		if (!e.mVariants.empty()) {
			ImGui::Spacing();
			TextDisabledWrapped(VDTextWToU8(e.mVariants[0].mPath).c_str());
		}

		// --- Provenance -----------------------------------------------
		// Small, but it is the difference between "the emulator invented
		// this" and "this came from a database record".
		if (m.mStatus == GameMetaStatus::Matched
			|| m.mStatus == GameMetaStatus::UserEdited)
		{
			ImGui::Spacing();
			if (m.mMatchedCRC32)
				ImGui::TextDisabled("matched by CRC %08X",
					(unsigned)m.mMatchedCRC32);
			if (m.mProviderGameId)
				ImGui::TextDisabled("screenscraper #%u",
					(unsigned)m.mProviderGameId);
			if (m.mFetchedTime) {
				char buf[64];
				const time_t tt = (time_t)m.mFetchedTime;
				struct tm tmv;
#ifdef _WIN32
				localtime_s(&tmv, &tt);
#else
				localtime_r(&tt, &tmv);
#endif
				strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tmv);
				ImGui::TextDisabled("fetched %s", buf);
			}
			if (m.mStatus == GameMetaStatus::UserEdited)
				ImGui::TextDisabled("edited by you - downloads keep your text");
		}
	}
	ImGui::EndChild();

	ImGui::Separator();

	// --- Pinned actions ---------------------------------------------
	ImGui::BeginDisabled(!canLaunch || !onLaunch);
	if (ImGui::Button("Play", ImVec2(-FLT_MIN, 0)) && onLaunch)
		onLaunch(userData, entryIndex);
	ImGui::EndDisabled();

	// "Download" on its own is ambiguous next to Play — download what,
	// the game?  Name the noun, and use the same wording as the Gaming
	// Mode sheet so the two frontends read alike.
	const float gap = ImGui::GetStyle().ItemSpacing.x;
	const bool haveMeta = (m.mStatus != GameMetaStatus::None);
	const char *fetchLabel = haveMeta ? "Update Metadata" : "Get Metadata";

	// The fetch button carries a much longer label than its neighbours,
	// so the row is split by weight rather than into equal thirds.
	const float rowW = ImGui::GetContentRegionAvail().x;
	const float fetchW = haveMeta
		? (rowW - gap * 2.0f) * 0.5f
		: (rowW - gap) * 0.6f;
	const float restW = haveMeta
		? (rowW - gap * 2.0f - fetchW) * 0.5f
		: (rowW - gap - fetchW);

	ImGui::BeginDisabled(!canDownload);
	if (ImGui::Button(fetchLabel, ImVec2(fetchW, 0)))
		ATUIMetadataStartDownloadForEntry(lib, entryIndex);
	ImGui::EndDisabled();
	if (!canDownload && !whyNot.empty()
		&& ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
	{
		ImGui::SetTooltip("%s", whyNot.c_str());
	}

	ImGui::SameLine(0, gap);
	if (ImGui::Button("Remove...", ImVec2(restW, 0)))
		ATUIMetadataOpenRemove(lib, entryIndex);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Remove this game from the library.\n"
			"The file itself is left alone.");

	if (haveMeta) {
		ImGui::SameLine(0, gap);
		if (ImGui::Button("Clear", ImVec2(restW, 0)))
			ATUIMetadataClearEntry(lib, entryIndex);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Discard the downloaded metadata and "
				"artwork for this game.");
	}
}

// ---------------------------------------------------------------------------
// Desktop: remove-from-library confirmation
// ---------------------------------------------------------------------------

void ATUIMetadataOpenRemove(ATGameLibrary& lib, int entryIndex) {
	const auto& entries = lib.GetEntries();
	if (entryIndex < 0 || (size_t)entryIndex >= entries.size())
		return;

	g_openRemove = true;
	g_removeEntry = entryIndex;
	g_removeName = VDTextWToU8(entries[entryIndex].mDisplayName);
	g_removePath = entries[entryIndex].mVariants.empty()
		? VDStringW() : entries[entryIndex].mVariants[0].mPath;
}

void ATUIRenderMetadataRemoveConfirm(ATGameLibrary& lib) {
	if (g_openRemove) {
		g_openRemove = false;
		ImGui::OpenPopup("Remove from library?##meta");
	}

	ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
		ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

	if (!ImGui::BeginPopupModal("Remove from library?##meta", nullptr,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
	{
		return;
	}

	// Re-find by path: a scan landing while the confirmation is open
	// would otherwise leave the index pointing at a different game, and
	// "yes, remove it" would remove the wrong one.
	auto& entries = lib.GetEntries();
	if (g_removeEntry < 0 || (size_t)g_removeEntry >= entries.size()
		|| entries[g_removeEntry].mVariants.empty()
		|| entries[g_removeEntry].mVariants[0].mPath != g_removePath)
	{
		g_removeEntry = g_removePath.empty()
			? -1 : lib.FindEntryByVariantPath(g_removePath);
	}

	if (g_removeEntry < 0) {
		ImGui::TextWrapped("That game is no longer in the library.");
		ImGui::Spacing();
		if (ImGui::Button("Close", ImVec2(110, 0)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}

	ImGui::TextWrapped("Remove \"%s\" from the library?", g_removeName.c_str());
	ImGui::Spacing();
	// The single most important sentence in this dialog.  "Remove" next
	// to a file name reads as "delete" to plenty of people, and being
	// wrong about that is unrecoverable.
	ImGui::TextDisabled("The file stays on disk. Only the library entry "
		"is removed.");
	// Say up front how long "removed" lasts for this particular game.
	// Promising more than we deliver is worse than the limitation.
	if (lib.WillRescanRestore((size_t)g_removeEntry)) {
		ImGui::TextDisabled("It sits inside a folder you scan, so a later "
			"rescan will find it again. Remove that folder under Sources "
			"to keep it out for good.");
	}
	ImGui::Spacing();

	ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.60f, 0.18f, 0.18f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.22f, 0.22f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.50f, 0.14f, 0.14f, 1.0f));
	if (ImGui::Button("Remove", ImVec2(130, 0))) {
		lib.RemoveEntry((size_t)g_removeEntry);
		GameBrowser_Invalidate();
		g_removeEntry = -1;
		g_removePath.clear();
		ImGui::CloseCurrentPopup();
	}
	ImGui::PopStyleColor(3);

	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(130, 0)))
		ImGui::CloseCurrentPopup();

	ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Desktop: Metadata tab
// ---------------------------------------------------------------------------

void ATUIRenderMetadataTab(ATGameLibrary& lib) {
	SeedCredentialBuffers();
	ATUIMetadataPumpAccountTest();

	ATMetadataSettings& s = ATMetadataGetSettings();
	bool dirty = false;

	// --- Provider -----------------------------------------------------
	ImGui::TextDisabled("Provider");
	ImGui::TextWrapped("ScreenScraper.fr  -  community database of retro "
		"game covers, screenshots and descriptions.");
	ImGui::TextDisabled("Atari 8-bit (system 43) and Atari 5200 (system 40)");

	if (!ATHttp::Available()) {
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.35f, 1.0f));
		ImGui::TextWrapped("%s", ATScreenScraperOutcomeText(
			ATScreenScraperOutcome::Unavailable));
		ImGui::PopStyleColor();
	}

	ImGui::Spacing();
	ImGui::Separator();

	// --- Account ------------------------------------------------------
	ImGui::TextDisabled("Account");

	int accountMode = s.mbUseUserAccount ? 1 : 0;
	if (ImGui::RadioButton("Anonymous", &accountMode, 0)) {
		s.mbUseUserAccount = false;
		dirty = true;
	}
	ImGui::SameLine();
	ImGui::TextDisabled("(shared allowance, slow, good for a few games)");

	if (ImGui::RadioButton("My ScreenScraper account", &accountMode, 1)) {
		s.mbUseUserAccount = true;
		dirty = true;
	}
	ImGui::SameLine();
	ImGui::TextDisabled("(faster, much larger allowance)");

	ImGui::Indent();
	{
		const bool disabled = !s.mbUseUserAccount;
		if (disabled)
			ImGui::BeginDisabled();

		// Commit on deactivate, not on every keystroke: the save path
		// writes the registry AND flushes it to disk, which is far too
		// heavy to run once per typed character.
		ImGui::PushItemWidth(240.0f);
		ImGui::InputText("Username", g_userNameBuf, sizeof g_userNameBuf);
		if (ImGui::IsItemDeactivatedAfterEdit())
			dirty = true;

		ImGuiInputTextFlags pwFlags = g_showPassword
			? 0 : ImGuiInputTextFlags_Password;
		ImGui::InputText("Password", g_passwordBuf, sizeof g_passwordBuf,
			pwFlags);
		if (ImGui::IsItemDeactivatedAfterEdit())
			dirty = true;
		ImGui::PopItemWidth();

		ImGui::SameLine();
		ImGui::Checkbox("Show", &g_showPassword);

		const bool testing =
			ATUIMetadataGetTestState() == ATMetadataTestState::Running;
		if (testing)
			ImGui::BeginDisabled();
		if (ImGui::Button("Test credentials", ImVec2(150, 0))) {
			CommitCredentials();
			ATUIMetadataStartAccountTest();
		}
		if (testing)
			ImGui::EndDisabled();

		const VDStringA testMessage = ATUIMetadataGetTestMessage();
		if (!testMessage.empty()) {
			ImGui::SameLine();
			const ATMetadataTestState state = ATUIMetadataGetTestState();
			ImVec4 colour(0.6f, 0.6f, 0.6f, 1.0f);
			if (state == ATMetadataTestState::Success)
				colour = ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
			else if (state == ATMetadataTestState::Failure)
				colour = ImVec4(0.95f, 0.55f, 0.45f, 1.0f);
			ImGui::TextColored(colour, "%s", testMessage.c_str());
		}

		if (disabled)
			ImGui::EndDisabled();
	}
	ImGui::Unindent();

	// The explainer opens itself the first time the tab is visited, and
	// again whenever a quota banner is showing — those are exactly the
	// moments the user needs it.
	if (!g_helpAutoOpened) {
		g_helpAutoOpened = true;
		g_helpOpen = !s.mbUseUserAccount;
	}
	ImGui::SetNextItemOpen(g_helpOpen, ImGuiCond_Always);
	if (ImGui::CollapsingHeader(kATMetadataAccountHelpTitle)) {
		g_helpOpen = true;
		ImGui::Indent();
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(kATMetadataAccountHelpBody);
		ImGui::PopTextWrapPos();
		if (ImGui::Button("Copy link", ImVec2(110, 0)))
			ImGui::SetClipboardText(kATMetadataProviderUrl);
		ImGui::SameLine();
		if (ImGui::Button("Open in browser", ImVec2(140, 0)))
			SDL_OpenURL(kATMetadataProviderUrl);
		ImGui::Unindent();
	} else {
		g_helpOpen = false;
	}

	ImGui::Spacing();
	ImGui::Separator();

	// --- What to download ---------------------------------------------
	ImGui::TextDisabled("What to download");
	if (ImGui::Checkbox("Text (title, description, publisher, year, genre)",
		&s.mbDownloadText)) dirty = true;
	if (ImGui::Checkbox("Box art", &s.mbDownloadBoxArt))         dirty = true;
	ImGui::SameLine(200);
	if (ImGui::Checkbox("Title screen", &s.mbDownloadTitleShot)) dirty = true;
	if (ImGui::Checkbox("Screenshot", &s.mbDownloadScreenshot))  dirty = true;
	ImGui::SameLine(200);
	if (ImGui::Checkbox("Logo / wheel", &s.mbDownloadLogo))      dirty = true;

	ImGui::Spacing();
	ImGui::PushItemWidth(200.0f);
	if (ImGui::Combo("Artwork shown", &s.mArtSlot,
		kATMetadataArtSlotNames, kATMetadataArtSlotCount))
	{
		dirty = true;
		GameBrowser_Invalidate();
	}
	ImGui::PopItemWidth();
	ImGui::SameLine();
	ImGui::TextDisabled("(applies to the whole library, right away)");

	ImGui::Spacing();
	ImGui::Separator();

	// --- Preferences --------------------------------------------------
	ImGui::TextDisabled("Preferences");

	int regionIdx = ATMetadataFindRegionIndex(s.mRegion.c_str());
	ImGui::PushItemWidth(200.0f);
	if (ImGui::Combo("Region", &regionIdx, kATMetadataRegionNames,
		kATMetadataRegionCount))
	{
		s.mRegion = kATMetadataRegionCodes[regionIdx];
		dirty = true;
	}

	int languageIdx = ATMetadataFindLanguageIndex(s.mLanguage.c_str());
	if (ImGui::Combo("Language", &languageIdx, kATMetadataLanguageNames,
		kATMetadataLanguageCount))
	{
		s.mLanguage = kATMetadataLanguageCodes[languageIdx];
		dirty = true;
	}
	ImGui::PopItemWidth();

	if (ImGui::Checkbox("Also try the Atari 5200 database when a cartridge "
		"isn't found", &s.mbTry5200Fallback)) dirty = true;
	if (ImGui::Checkbox("Fetch metadata for newly added games automatically",
		&s.mbAutoFetchNewGames)) dirty = true;
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"When games appear in the library, look them up without\n"
			"being asked.\n\n"
			"Applies to up to %d new games at a time. A larger import is\n"
			"left for you to start yourself, so a first scan of a big\n"
			"collection never spends the shared allowance unasked.",
			(int)kATMetadataAutoFetchMax);
	}
	if (ImGui::Checkbox("Search by name when the file cannot be identified",
		&s.mbFuzzyNameMatch)) dirty = true;
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Most 8-bit files are cracked, trained or renamed, so their\n"
			"checksum and file name match nothing in the database.\n"
			"This searches for the cleaned-up title instead.\n\n"
			"Costs two extra requests per unidentified game, and a\n"
			"close-but-wrong match is possible - titles must score 85%%\n"
			"or better to be accepted.");
	}

	ImGui::Spacing();
	ImGui::Separator();

	// --- Advanced -----------------------------------------------------
	// Only worth showing when the build has no credential of its own —
	// otherwise it is a footgun with no upside.
	if (!ATMetadataHaveBakedDevCredential()) {
		ImGui::SetNextItemOpen(g_showAdvanced, ImGuiCond_Always);
		if (ImGui::CollapsingHeader("Advanced: developer credential")) {
			g_showAdvanced = true;
			ImGui::Indent();
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(
				"This build of AltirraSDL ships without a ScreenScraper "
				"developer credential, so downloads are disabled.  You can "
				"request one from the ScreenScraper forum and enter it "
				"here.  This is separate from your own account above.");
			ImGui::PopTextWrapPos();
			ImGui::PushItemWidth(240.0f);
			ImGui::InputText("Developer ID", g_devIdBuf, sizeof g_devIdBuf);
			if (ImGui::IsItemDeactivatedAfterEdit())
				dirty = true;
			ImGui::InputText("Developer password", g_devPasswordBuf,
				sizeof g_devPasswordBuf, ImGuiInputTextFlags_Password);
			if (ImGui::IsItemDeactivatedAfterEdit())
				dirty = true;
			ImGui::PopItemWidth();
			ImGui::Unindent();
		} else {
			g_showAdvanced = false;
		}
		ImGui::Separator();
	}

	// --- Storage ------------------------------------------------------
	ImGui::TextDisabled("Storage");
	{
		uint64_t bytes = 0;
		int files = 0;
		ATUIMetadataGetStorageUsage(lib, bytes, files);
		ImGui::Text("%d file%s  -  %s", files, files == 1 ? "" : "s",
			FormatBytes(bytes).c_str());
	}

	if (ImGui::Button("Open folder", ImVec2(130, 0))) {
		VDStringA url("file://");
		url += MediaDir(lib);
		SDL_OpenURL(url.c_str());
	}

	ImGui::SameLine();
	if (ImGui::Button("Delete downloaded media", ImVec2(200, 0))) {
		ATUIConfirmOptions opts;
		opts.title = "Delete downloaded media";
		opts.message = "Delete every downloaded cover and screenshot?\n"
			"Titles, descriptions and other text are kept.";
		opts.confirmLabel = "Delete";
		opts.destructive = true;
		ATGameLibrary *libPtr = &lib;
		opts.onConfirm = [libPtr]() { ATUIMetadataDeleteAllMedia(*libPtr); };
		ATUIShowConfirm(std::move(opts));
	}

	ImGui::SameLine();
	if (ImGui::Button("Clear all metadata", ImVec2(170, 0))) {
		ATUIConfirmOptions opts;
		opts.title = "Clear all metadata";
		opts.message = "Remove all downloaded metadata and media from every "
			"game in the library?\nYour games and play history are not "
			"affected.";
		opts.confirmLabel = "Clear";
		opts.destructive = true;
		ATGameLibrary *libPtr = &lib;
		opts.onConfirm = [libPtr]() { ATUIMetadataClearAll(*libPtr); };
		ATUIShowConfirm(std::move(opts));
	}

	if (dirty) {
		CommitCredentials();   // also persists everything else
	}
}
