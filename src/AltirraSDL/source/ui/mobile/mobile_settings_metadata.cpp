//	AltirraSDL - Gaming Mode: Settings -> Game Library -> Metadata
//
//	The touch/gamepad counterpart to the Desktop dialog's Metadata tab.
//	Same options, same order, same wording — the two frontends share the
//	settings struct and the explainer copy so they cannot drift apart.
//
//	Rendered inside RenderSettings()'s scroll child, which is already
//	NavFlattened and already driving ATTouchDragScroll, so this file only
//	emits rows.  It must not open a window, a child, or a drag-scroll
//	scope of its own.

#include <stdafx.h>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <cstdio>

#include <vd2/system/text.h>

#include "ui_mobile.h"
#include "mobile_internal.h"
#include "ui_main.h"
#include "touch_widgets.h"
#include "altirra_icons.h"
#include "ui/gamelibrary/game_library.h"
#include "ui/dialogs/ui_game_metadata.h"
#include "media/metadata_scraper.h"
#include "media/metadata_settings.h"
#include "media/http_client.h"

extern ATGameLibrary *GetGameLibrary();

extern void GameBrowser_Invalidate();

namespace {

// Text-entry targets.  Gaming Mode has no separate text screen: an
// ImGui InputText raises the platform IME automatically (the SDL3
// backend's Platform_SetImeData hook), and the scroll-aware wrapper
// keeps the field above the keyboard.
char s_userBuf[128] = {};
char s_passBuf[128] = {};
char s_devIdBuf[128] = {};
char s_devPassBuf[128] = {};
bool s_buffersSeeded = false;
bool s_showPassword = false;

// True while any option picker was on screen last frame.  The settings
// screen's Back handler reads this to stand down, so Back closes the
// picker rather than also popping the page.  A one-frame lag is
// harmless: the picker lives for many frames, and the frame it opens on
// is not a frame the user is also pressing Back.
bool s_pickerVisible = false;

void SeedBuffers() {
	if (s_buffersSeeded)
		return;
	s_buffersSeeded = true;
	const ATMetadataSettings &s = ATMetadataGetSettings();
	std::snprintf(s_userBuf, sizeof s_userBuf, "%s", s.mUserName.c_str());
	std::snprintf(s_passBuf, sizeof s_passBuf, "%s", s.mUserPassword.c_str());
	std::snprintf(s_devIdBuf, sizeof s_devIdBuf, "%s", s.mCustomDevId.c_str());
	std::snprintf(s_devPassBuf, sizeof s_devPassBuf, "%s",
		s.mCustomDevPassword.c_str());
}

void CommitAndSave() {
	ATMetadataSettings &s = ATMetadataGetSettings();
	s.mUserName = s_userBuf;
	s.mUserPassword = s_passBuf;
	s.mCustomDevId = s_devIdBuf;
	s.mCustomDevPassword = s_devPassBuf;
	ATMetadataSaveSettings();
}

// Full-screen option list.
//
// Focus deliberately starts on the CURRENT value, not on the first row.
// Landing on row 0 every time means a gamepad user has to count
// D-pad presses back to where they were, and it makes "confirm the
// current setting" cost N presses instead of one.
bool OptionPickerModal(const char *title, const char *const *items,
	int count, int currentIdx, int *outIdx, bool *outVisible)
{
	bool changed = false;

	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
		ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	const ImGuiIO &io = ImGui::GetIO();
	const float popupW = io.DisplaySize.x * 0.9f < dp(520.0f)
		? io.DisplaySize.x * 0.9f : dp(520.0f);
	const float popupH = io.DisplaySize.y * 0.8f < dp(560.0f)
		? io.DisplaySize.y * 0.8f : dp(560.0f);
	ImGui::SetNextWindowSize(ImVec2(popupW, popupH), ImGuiCond_Appearing);

	if (ImGui::BeginPopupModal(title, nullptr,
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse
		| ImGuiWindowFlags_NoResize))
	{
		ImGui::BeginChild("##optlist", ImVec2(0, -dp(56.0f)),
			ImGuiChildFlags_NavFlattened);
		ATTouchDragScroll();

		for (int i = 0; i < count; ++i) {
			ImGui::PushID(i);
			const bool selected = (i == currentIdx);
			if (ATTouchListItem(items[i], nullptr, selected, false)) {
				*outIdx = i;
				changed = true;
				ImGui::CloseCurrentPopup();
			}
			if (selected)
				ImGui::SetItemDefaultFocus();
			ImGui::PopID();
			ImGui::Dummy(ImVec2(0, dp(4.0f)));
		}

		ATTouchEndDragScroll();
		ImGui::EndChild();

		if (ATTouchButton("Cancel", ImVec2(-FLT_MIN,
			dp(ATTouch::kButtonHeightNormal)), ATTouchButtonStyle::Subtle))
		{
			ImGui::CloseCurrentPopup();
		}

		// Back/Escape closes the picker without changing anything.  The
		// enclosing settings page gates its own Back handling on
		// IsPopupOpen, so this cannot pop two levels at once.
		if (!ImGui::IsAnyItemActive()
			&& (ImGui::IsKeyPressed(ImGuiKey_Escape, false)
				|| ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
		{
			ImGui::CloseCurrentPopup();
		}

		*outVisible = true;
		ImGui::EndPopup();
	}

	return changed;
}

VDStringA FormatBytes(uint64_t bytes) {
	char buf[64];
	if (bytes >= 1024ull * 1024ull * 1024ull)
		std::snprintf(buf, sizeof buf, "%.1f GB",
			(double)bytes / (1024.0 * 1024.0 * 1024.0));
	else if (bytes >= 1024ull * 1024ull)
		std::snprintf(buf, sizeof buf, "%.1f MB",
			(double)bytes / (1024.0 * 1024.0));
	else if (bytes >= 1024ull)
		std::snprintf(buf, sizeof buf, "%.1f KB", (double)bytes / 1024.0);
	else
		std::snprintf(buf, sizeof buf, "%u bytes", (unsigned)bytes);
	return VDStringA(buf);
}

}  // namespace

void RenderSettingsPage_Metadata(ATMobileUIState &mobileState) {
	(void)mobileState;

	SeedBuffers();
	ATUIMetadataPumpAccountTest();

	ATGameLibrary *libp = GetGameLibrary();
	if (!libp) {
		GameBrowser_Init();
		libp = GetGameLibrary();
	}
	if (!libp) {
		ATTouchMutedText("Game Library is not available.");
		return;
	}
	ATGameLibrary &lib = *libp;

	ATMetadataSettings &s = ATMetadataGetSettings();
	ATMetadataScraper &scraper = ATMetadataGetScraper();
	bool dirty = false;

	const float rowH = dp(ATTouch::kButtonHeightNormal);

	// --- Availability -------------------------------------------------
	VDStringA whyNot;
	const bool usable = ATUIMetadataIsUsable(whyNot);
	if (!usable) {
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextColored(ATMobileCol(ATMobileGetPalette().warning),
			"%s", whyNot.c_str());
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0, dp(8.0f)));
	}

	// --- Download actions ---------------------------------------------
	ATTouchSection("Download");

	if (scraper.IsRunning()) {
		char status[160];
		std::snprintf(status, sizeof status,
			"Downloading  %d / %d   (%d found, %d missing)",
			scraper.GetDone(), scraper.GetTotal(),
			scraper.GetMatched(), scraper.GetNotFound());
		ATTouchMutedText(status);
		if (ATTouchButton("Cancel download", ImVec2(-1, rowH),
			ATTouchButtonStyle::Danger, ICON_MD_CLOSE))
		{
			scraper.Cancel();
		}
	} else {
		const int missing = ATMetadataCountEntries(lib, true);
		const int all = ATMetadataCountEntries(lib, false);

		ImGui::BeginDisabled(!usable || lib.IsScanning() || missing == 0);
		char label[96];
		std::snprintf(label, sizeof label,
			"Download what's missing  (%d)##dlmissing", missing);
		if (ATTouchButton(label, ImVec2(-1, rowH),
			ATTouchButtonStyle::Accent, ICON_MD_CLOUD_DOWNLOAD))
		{
			ATUIMetadataStartDownload(lib, true);
		}
		ImGui::EndDisabled();

		ImGui::Dummy(ImVec2(0, dp(6.0f)));

		ImGui::BeginDisabled(!usable || lib.IsScanning() || all == 0);
		std::snprintf(label, sizeof label,
			"Re-download everything  (%d)##dlall", all);
		if (ATTouchButton(label, ImVec2(-1, rowH),
			ATTouchButtonStyle::Neutral, ICON_MD_REFRESH))
		{
			ATMobileUI_ShowConfirmDialog("Re-download everything",
				"Download metadata again for every game in the library?",
				[&lib]() { ATUIMetadataStartDownload(lib, false); });
		}
		ImGui::EndDisabled();

		if (lib.IsScanning())
			ATTouchMutedText("Wait for the library scan to finish.");

		const VDStringA banner = scraper.GetBanner();
		if (!banner.empty()) {
			ImGui::Dummy(ImVec2(0, dp(6.0f)));
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextColored(ATMobileCol(ATMobileGetPalette().warning),
				"%s", banner.c_str());
			ImGui::PopTextWrapPos();
			if (ATTouchButton("Dismiss##banner", ImVec2(-1, dp(40.0f)),
				ATTouchButtonStyle::Subtle))
			{
				scraper.ClearBanner();
			}
		}
	}

	// Coverage summary — the answer to "did that actually do anything?"
	{
		int matched = 0, notFound = 0, none = 0;
		for (const auto &e : lib.GetEntries()) {
			switch (e.mMeta.mStatus) {
				case GameMetaStatus::Matched:
				case GameMetaStatus::UserEdited: ++matched; break;
				case GameMetaStatus::NotFound:   ++notFound; break;
				default:                          ++none; break;
			}
		}
		char summary[128];
		std::snprintf(summary, sizeof summary,
			"%d with metadata  \xC2\xB7  %d not found  \xC2\xB7  %d not tried",
			matched, notFound, none);
		ATTouchMutedText(summary);
	}

	// --- Account ------------------------------------------------------
	ATTouchSection("Account");

	bool useAccount = s.mbUseUserAccount;
	if (ATTouchToggle("Use my ScreenScraper account", &useAccount)) {
		s.mbUseUserAccount = useAccount;
		dirty = true;
	}
	ATTouchMutedText(s.mbUseUserAccount
		? "Faster, and a much larger daily allowance."
		: "Anonymous: a small allowance shared with all AltirraSDL users.");

	if (s.mbUseUserAccount) {
		ImGui::Dummy(ImVec2(0, dp(4.0f)));
		ATTouchMutedText("Username");
		// Commit on deactivate, not per keystroke: saving writes the
		// registry and flushes it to disk, which must not happen once
		// per typed character (and on Android that is a real cost).
		ImGui::PushItemWidth(-FLT_MIN);
		ATTouchInputTextScrollAware("##ssuser", s_userBuf, sizeof s_userBuf);
		if (ImGui::IsItemDeactivatedAfterEdit())
			dirty = true;

		ATTouchMutedText("Password");
		ATTouchInputTextScrollAware("##sspass", s_passBuf, sizeof s_passBuf,
			s_showPassword ? 0 : ImGuiInputTextFlags_Password);
		if (ImGui::IsItemDeactivatedAfterEdit())
			dirty = true;
		ImGui::PopItemWidth();

		bool show = s_showPassword;
		if (ATTouchToggle("Show password", &show))
			s_showPassword = show;

		const bool testing =
			ATUIMetadataGetTestState() == ATMetadataTestState::Running;
		ImGui::BeginDisabled(testing);
		if (ATTouchButton(testing ? "Checking..." : "Test credentials",
			ImVec2(-1, rowH), ATTouchButtonStyle::Neutral, ICON_MD_CHECK))
		{
			CommitAndSave();
			ATUIMetadataStartAccountTest();
		}
		ImGui::EndDisabled();

		const VDStringA testMessage = ATUIMetadataGetTestMessage();
		if (!testMessage.empty()) {
			const ATMetadataTestState state = ATUIMetadataGetTestState();
			const ATMobilePalette &pal = ATMobileGetPalette();
			uint32 colour = pal.textMuted;
			if (state == ATMetadataTestState::Success)
				colour = pal.success;
			else if (state == ATMetadataTestState::Failure)
				colour = pal.danger;
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextColored(ATMobileCol(colour), "%s", testMessage.c_str());
			ImGui::PopTextWrapPos();
		}
	}

	ImGui::Dummy(ImVec2(0, dp(4.0f)));
	if (ATTouchListItem(kATMetadataAccountHelpTitle,
		"Free, and lifts the download limit", false, true, ICON_MD_HELP_OUTLINE))
	{
		ATMobileUI_ShowInfoModal(kATMetadataAccountHelpTitle,
			kATMetadataAccountHelpBody);
	}

	// --- What to download ---------------------------------------------
	ATTouchSection("What to download");

	bool v = s.mbDownloadText;
	if (ATTouchToggle("Titles and descriptions", &v)) { s.mbDownloadText = v; dirty = true; }
	v = s.mbDownloadBoxArt;
	if (ATTouchToggle("Box art", &v))                 { s.mbDownloadBoxArt = v; dirty = true; }
	v = s.mbDownloadTitleShot;
	if (ATTouchToggle("Title screen", &v))            { s.mbDownloadTitleShot = v; dirty = true; }
	v = s.mbDownloadScreenshot;
	if (ATTouchToggle("Screenshot", &v))              { s.mbDownloadScreenshot = v; dirty = true; }
	v = s.mbDownloadLogo;
	if (ATTouchToggle("Logo / wheel", &v))            { s.mbDownloadLogo = v; dirty = true; }

	if (ATTouchListItem("Artwork shown",
		kATMetadataArtSlotNames[s.mArtSlot], false, true))
	{
		ImGui::OpenPopup("Artwork shown##pick");
	}

	// --- Preferences --------------------------------------------------
	ATTouchSection("Preferences");

	const int regionIdx = ATMetadataFindRegionIndex(s.mRegion.c_str());
	if (ATTouchListItem("Region", kATMetadataRegionNames[regionIdx],
		false, true))
	{
		ImGui::OpenPopup("Region##pick");
	}

	const int languageIdx = ATMetadataFindLanguageIndex(s.mLanguage.c_str());
	if (ATTouchListItem("Language", kATMetadataLanguageNames[languageIdx],
		false, true))
	{
		ImGui::OpenPopup("Language##pick");
	}

	v = s.mbTry5200Fallback;
	if (ATTouchToggle("Also try the Atari 5200 database", &v)) {
		s.mbTry5200Fallback = v;
		dirty = true;
	}
	v = s.mbAutoFetchNewGames;
	if (ATTouchToggle("Fetch for new games automatically", &v)) {
		s.mbAutoFetchNewGames = v;
		dirty = true;
	}
	{
		char hint[192];
		snprintf(hint, sizeof hint,
			"Games that appear in the library are looked up without being "
			"asked, up to %d at a time. A bigger import is left for you to "
			"start below.", (int)kATMetadataAutoFetchMax);
		ATTouchMutedText(hint);
	}

	v = s.mbFuzzyNameMatch;
	if (ATTouchToggle("Search by name if not identified", &v)) {
		s.mbFuzzyNameMatch = v;
		dirty = true;
	}
	ATTouchMutedText("Cracked and renamed files match nothing by "
		"checksum. Searching for the cleaned-up title finds many of "
		"them, at two extra requests each.");


	// --- Advanced: developer credential -------------------------------
	if (!ATMetadataHaveBakedDevCredential()) {
		ATTouchSection("Advanced");
		ATTouchMutedText("This build ships without a ScreenScraper "
			"developer credential, so downloads are disabled. You can "
			"request one from the ScreenScraper forum and enter it here. "
			"This is separate from your own account above.");
		ImGui::PushItemWidth(-FLT_MIN);
		ATTouchMutedText("Developer ID");
		ATTouchInputTextScrollAware("##devid", s_devIdBuf, sizeof s_devIdBuf);
		if (ImGui::IsItemDeactivatedAfterEdit())
			dirty = true;
		ATTouchMutedText("Developer password");
		ATTouchInputTextScrollAware("##devpass", s_devPassBuf,
			sizeof s_devPassBuf, ImGuiInputTextFlags_Password);
		if (ImGui::IsItemDeactivatedAfterEdit())
			dirty = true;
		ImGui::PopItemWidth();
	}

	// --- Storage ------------------------------------------------------
	ATTouchSection("Storage");
	{
		uint64_t bytes = 0;
		int files = 0;
		ATUIMetadataGetStorageUsage(lib, bytes, files);
		char buf[96];
		std::snprintf(buf, sizeof buf, "%d file%s  \xC2\xB7  %s",
			files, files == 1 ? "" : "s", FormatBytes(bytes).c_str());
		ATTouchMutedText(buf);
	}

	if (ATTouchButton("Delete downloaded media", ImVec2(-1, rowH),
		ATTouchButtonStyle::Danger, ICON_MD_DELETE))
	{
		ATMobileUI_ShowConfirmDialog("Delete downloaded media",
			"Delete every downloaded cover and screenshot?\n"
			"Titles and descriptions are kept.",
			[&lib]() { ATUIMetadataDeleteAllMedia(lib); });
	}

	ImGui::Dummy(ImVec2(0, dp(6.0f)));

	if (ATTouchButton("Clear all metadata", ImVec2(-1, rowH),
		ATTouchButtonStyle::Danger, ICON_MD_DELETE_FOREVER))
	{
		ATMobileUI_ShowConfirmDialog("Clear all metadata",
			"Remove all downloaded metadata and media from every game?\n"
			"Your games and play history are not affected.",
			[&lib]() { ATUIMetadataClearAll(lib); });
	}

	// --- Pickers ------------------------------------------------------
	{
		int picked = 0;
		bool visible = false;
		if (OptionPickerModal("Region##pick", kATMetadataRegionNames,
			kATMetadataRegionCount, regionIdx, &picked, &visible))
		{
			s.mRegion = kATMetadataRegionCodes[picked];
			dirty = true;
		}
		if (OptionPickerModal("Language##pick", kATMetadataLanguageNames,
			kATMetadataLanguageCount, languageIdx, &picked, &visible))
		{
			s.mLanguage = kATMetadataLanguageCodes[picked];
			dirty = true;
		}
		if (OptionPickerModal("Artwork shown##pick", kATMetadataArtSlotNames,
			kATMetadataArtSlotCount, s.mArtSlot, &picked, &visible))
		{
			s.mArtSlot = picked;
			dirty = true;
			// Live preference: every tile in the library changes with it.
			GameBrowser_Invalidate();
		}
		s_pickerVisible = visible;
	}

	if (dirty)
		CommitAndSave();
}

bool ATMobileMetadataPickerOpen() {
	return s_pickerVisible;
}
