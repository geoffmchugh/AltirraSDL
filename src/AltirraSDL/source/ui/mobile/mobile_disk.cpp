//	AltirraSDL - Mobile UI (split from ui_mobile.cpp Phase 3b)
//	Verbatim move; helpers/state shared via mobile_internal.h.

#include <stdafx.h>
#include <cwctype>
#include <vector>
#include <algorithm>
#include <functional>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <vd2/system/vdtypes.h>
#include <vd2/system/VDString.h>
#include <vd2/system/text.h>
#include <vd2/system/file.h>
#include <vd2/system/filesys.h>
#include <vd2/system/registry.h>
#include <vd2/system/error.h>
#include <at/atcore/media.h>
#include <at/atio/image.h>

#include "ui_mobile.h"
#include "ui_main.h"
#include "touch_controls.h"
#include "touch_widgets.h"
#include "../../app/disk_state.h"  // ATResolveDiskMount
#include "simulator.h"
#include "gtia.h"
#include <at/ataudio/pokey.h>
#include "diskinterface.h"
#include "disk.h"
#include <at/atio/diskimage.h>
#include "mediamanager.h"
#include "firmwaremanager.h"
#include "options.h"
#include "uiaccessors.h"
#include "uitypes.h"
#include "constants.h"
#include "display_backend.h"
#include "android_platform.h"
#include <at/ataudio/audiooutput.h>

#include "mobile_internal.h"
#include "altirra_icons.h"
#ifdef ALTIRRA_NETPLAY_ENABLED
#include "netplay/netplay_glue.h"
#endif

extern ATSimulator g_sim;
extern VDStringA ATGetConfigDir();
extern void ATRegistryFlushToDisk();
extern IDisplayBackend *ATUIGetDisplayBackend();

static const char *BasenameU8(const char *path) {
	const char *p = strrchr(path, '/');
	return p ? p + 1 : path;
}

// Gaming Mode deliberately exposes one saving choice for the entire disk
// subsystem.  Desktop Mode retains its per-drive selector for specialist
// multi-disk setups; a gamepad/touch user should not have to discover which
// drive contains a game's save disk before enabling persistence.
static const ATMediaWriteMode kMobileWriteModeValues[] = {
	kATMediaWriteMode_RO,
	kATMediaWriteMode_VRWSafe,
	kATMediaWriteMode_VRW,
	kATMediaWriteMode_RW,
};

static const char *const kMobileWriteModeLabels[] = {
	"Read-only", "Temporary", "Format", "Persistent",
};

static int GetMobileWriteModeIndex(ATMediaWriteMode mode) {
	for (int i = 0; i < (int)(sizeof(kMobileWriteModeValues)
		/ sizeof(kMobileWriteModeValues[0])); ++i) {
		if (kMobileWriteModeValues[i] == mode)
			return i;
	}

	return 1; // Preserve Altirra's Virtual R/W (Safe) fallback.
}

// Move a currently mounted virtual disk into its protected persistent slot
// before enabling R/W.  Setting the AutoFlush bit on the existing source
// mount would otherwise flush the game's changes directly to the user's
// original image.  SaveDiskAs also preserves any save data accumulated in
// the current virtual session.
static bool MoveMountedDiskToPersistentCopy(ATDiskInterface& diskIf,
	VDStringA& error)
{
	if (!diskIf.IsDiskLoaded())
		return true;

	const wchar_t *path = diskIf.GetPath();
	IATDiskImage *image = diskIf.GetDiskImage();
	if (!path || !*path || !image || image->IsDynamic()) {
		error = "This disk cannot be saved as a persistent image.";
		return false;
	}

	VDStringW persistentPath = ATResolveDiskMount(path,
		kATMediaWriteMode_RW);
	if (persistentPath.empty()) {
		error = "Altirra could not create a persistent copy for this disk.";
		return false;
	}

	if (wcscmp(persistentPath.c_str(), path) == 0) {
		// A path already inside disk_state is the protected working copy.
		// Any other unchanged path means that the resolver could not safely
		// redirect this source (for example, a dynamic or unsupported image).
		VDStringW canonicalPath = ATResolveDiskCanonical(path);
		if (wcscmp(canonicalPath.c_str(), path) == 0) {
			error = "Altirra could not create a protected persistent copy for this disk.";
			return false;
		}
		return true;
	}

	ATDiskImageFormat format = image->GetImageFormat();
	if (format == kATDiskImageFormat_None) {
		error = "This disk image format cannot be saved persistently.";
		return false;
	}

	try {
		diskIf.SaveDiskAs(persistentPath.c_str(), format);
	} catch (const MyError& e) {
		error.sprintf("Altirra could not save this disk's current data: %s",
			e.c_str());
		return false;
	}

	return true;
}

static void SetMobileGlobalWriteMode(ATSimulator& sim,
	ATMediaWriteMode writeMode)
{
#ifdef ALTIRRA_NETPLAY_ENABLED
	// The netplay boot path hardcodes VRWSafe and canonical image bytes.
	// Changing live drive modes during Handshaking or Lockstepping would
	// change drive status responses on only this peer and desync it.
	if (ATNetplayGlue::IsSessionEngaged())
		return;
#endif

	extern ATOptions g_ATOptions;
	ATOptions previous(g_ATOptions);
	g_ATOptions.mDefaultWriteMode = writeMode;
	if (g_ATOptions != previous) {
		g_ATOptions.mbDirty = true;
		ATOptionsRunUpdateCallbacks(&previous);
		ATOptionsSave();
		try {
			ATRegistryFlushToDisk();
		} catch (...) {
			// The normal suspend/exit path retries persistence.
		}
	}

	int failedDrive = -1;
	VDStringA error;
	for (int i = 0; i < 15; ++i) {
		ATDiskInterface& diskIf = sim.GetDiskInterface(i);
		const ATMediaWriteMode previousWriteMode = diskIf.GetWriteMode();

		// R/W mounts must use the disk-state working copy.  Do the move
		// before flipping the mode, so failures leave an inserted disk in
		// its prior safe mode instead of making it write to its source.
		if (writeMode == kATMediaWriteMode_RW
			&& !MoveMountedDiskToPersistentCopy(diskIf, error)) {
			if (failedDrive < 0)
				failedDrive = i;
			continue;
		}

		// SetWriteMode stops a pending auto-flush timer when moving away
		// from Persistent.  Flush first so a just-written in-game save is
		// not silently discarded during the saving-type change.
		if ((previousWriteMode & kATMediaWriteMode_AutoFlush)
			&& !(writeMode & kATMediaWriteMode_AutoFlush)) {
			diskIf.Flush();
		}

		diskIf.SetWriteMode(writeMode);
	}

	if (failedDrive >= 0) {
		VDStringA message;
		message.sprintf(
			"D%d kept its previous saving type.\n\n%s\n\n"
			"Newly mounted disks will use Persistent.",
			failedDrive + 1, error.c_str());
		ShowInfoModal("Persistent saving unavailable", message.c_str());
	}
}

void RenderMobileDiskRow(ATSimulator &sim, int driveIdx,
	ATMobileUIState &mobileState)
{
	ATDiskInterface &di = sim.GetDiskInterface(driveIdx);
	bool loaded = di.IsDiskLoaded();
	bool dirty  = loaded && di.IsDirty();

	ImGui::PushID(driveIdx);

	// Row background: gradient card, same recipe as the Settings home
	// category rows.  Matches the rest of the Gaming-Mode UI.
	float rowH = dp(80.0f);
	ImVec2 cursor = ImGui::GetCursorScreenPos();
	float availW = ImGui::GetContentRegionAvail().x;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ATMobilePalette &pal = ATMobileGetPalette();
	{
		ImVec2 cardBR(cursor.x + availW, cursor.y + rowH);
		ATMobileDrawGradientRect(cursor, cardBR,
			pal.cardBgTop, pal.cardBg, dp(10.0f));
		dl->AddRect(cursor, cardBR, pal.cardBorder, dp(10.0f), 0, 1.0f);
	}

	// --- Left column: drive label + filename ---
	float leftPad  = dp(16.0f);
	float rightPad = dp(16.0f);
	ImGui::SetCursorScreenPos(ImVec2(cursor.x + leftPad, cursor.y + dp(10.0f)));
	ImGui::SetWindowFontScale(1.25f);
	// Semantic warning colour for "modified", palette text otherwise.
	ImU32 labelCol = dirty ? pal.warning : pal.text;
	ImGui::PushStyleColor(ImGuiCol_Text, ATMobileCol(labelCol));
	ImGui::Text("D%d:", driveIdx + 1);
	ImGui::PopStyleColor();
	ImGui::SetWindowFontScale(1.0f);

	// Filename / status, one line below the drive label.
	ImGui::SetCursorScreenPos(ImVec2(cursor.x + leftPad, cursor.y + dp(36.0f)));
	if (loaded) {
		const wchar_t *path = di.GetPath();
		if (path && *path) {
			VDStringA u8 = VDTextWToU8(VDStringW(path));
			ImGui::PushStyleColor(ImGuiCol_Text, ATMobileCol(pal.text));
			ImGui::Text("%s", BasenameU8(u8.c_str()));
			ImGui::PopStyleColor();
		} else {
			ImGui::PushStyleColor(ImGuiCol_Text, ATMobileCol(pal.textMuted));
			ImGui::TextUnformatted("(loaded)");
			ImGui::PopStyleColor();
		}

		// Show "(modified)" tag if dirty.
		if (dirty) {
			ImGui::SetCursorScreenPos(ImVec2(cursor.x + leftPad, cursor.y + dp(58.0f)));
			ImGui::PushStyleColor(ImGuiCol_Text, ATMobileCol(pal.warning));
			ImGui::TextUnformatted("modified");
			ImGui::PopStyleColor();
		}
	} else {
		ImGui::PushStyleColor(ImGuiCol_Text, ATMobileCol(pal.textMuted));
		ImGui::TextUnformatted("(empty)");
		ImGui::PopStyleColor();
	}

	// --- Right column: Mount [+ Select] + Eject buttons ---
	// Select is only shown when the mounted disk belongs to a
	// multi-variant Game Library entry — see GameBrowser_FindEntryForPath.
	int gameEntry = -1;
	if (loaded) {
		const wchar_t *path = di.GetPath();
		if (path && *path)
			gameEntry = GameBrowser_FindEntryForPath(path);
	}
	const bool hasAlts = gameEntry >= 0
		&& GameBrowser_GetVariantCount(gameEntry) > 1;

	float btnW = dp(88.0f);
	float btnH = dp(48.0f);
	float btnGap = dp(8.0f);
	float btnY = cursor.y + (rowH - btnH) * 0.5f;
	float ejectX = cursor.x + availW - rightPad - btnW;
	float selectX = hasAlts ? (ejectX - btnGap - btnW) : ejectX;
	float mountX = hasAlts
		? (selectX - btnGap - btnW)
		: (ejectX - btnGap - btnW);

	ImGui::SetCursorScreenPos(ImVec2(mountX, btnY));
	if (ATTouchButton("Mount", ImVec2(btnW, btnH),
		ATTouchButtonStyle::Accent, ICON_MD_UPLOAD_FILE))
	{
		s_diskMountTargetDrive = driveIdx;
		s_romFolderMode = false;
		mobileState.currentScreen = ATMobileUIScreen::FileBrowser;
		s_fileBrowserNeedsRefresh = true;
	}

	if (hasAlts) {
		ImGui::SetCursorScreenPos(ImVec2(selectX, btnY));
		if (ATTouchButton("Side", ImVec2(btnW, btnH))) {
			int drive = driveIdx;
			GameBrowser_ShowVariantPickerForSwap(gameEntry,
				[drive](const VDStringW &variantPath) {
					ATDiskInterface &tgt =
						g_sim.GetDiskInterface(drive);
					try {
						// Disk-state interception (see disk_state.h):
						// when the drive's current write mode is R/W,
						// route the new variant through the helper so
						// the swap mounts the NEW image's working copy
						// instead of mutating the source.  No-op for
						// the other write modes.
						ATMediaWriteMode wm = tgt.GetWriteMode();
						VDStringW mountPath = ATResolveDiskMount(
							variantPath.c_str(), wm);

						// Route through ATSimulator::Load (matches
						// Windows uidisk.cpp:1060-1065).  The 1-arg
						// ATDiskInterface::LoadDisk path flags images
						// as non-updatable; see ui_main.cpp
						// kATDeferred_AttachDisk for the full note.
						// The Side button is only shown when the drive
						// already has a disk mounted, so inheriting the
						// current write mode keeps the same R/O vs.
						// R/W vs. VRW choice the user had.
						ATImageLoadContext ctx;
						ctx.mLoadType  = kATImageType_Disk;
						ctx.mLoadIndex = drive;
						g_sim.Load(mountPath.c_str(), wm, &ctx);
					} catch (const MyError &e) {
						ShowInfoModal("Mount Failed", e.c_str());
					}
				});
		}
	}

	ImGui::SetCursorScreenPos(ImVec2(ejectX, btnY));
	ImGui::BeginDisabled(!loaded);
	if (ATTouchButton("Eject", ImVec2(btnW, btnH),
			ATTouchButtonStyle::Neutral, ICON_MD_EJECT)) {
		try {
			di.UnloadDisk();
			sim.GetDiskDrive(driveIdx).SetEnabled(false);
		} catch (const MyError &e) {
			ShowInfoModal("Eject Failed", e.c_str());
		}
	}
	ImGui::EndDisabled();

	// Advance the cursor past the row for the next iteration
	ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + rowH + dp(8.0f)));
	ImGui::PopID();
}

void RenderMobileDiskManager(ATSimulator &sim, ATUIState &uiState,
	ATMobileUIState &mobileState, SDL_Window *window)
{
#ifdef ALTIRRA_NETPLAY_ENABLED
	// Match the Desktop Disk Drives dialog: disk mounting and configuration
	// are local-only mutations, so they must not remain reachable after a
	// peer has engaged in a lockstep session.
	if (ATNetplayGlue::IsSessionEngaged()) {
		mobileState.currentScreen = ATMobileUIScreen::HamburgerMenu;
		return;
	}
#endif

	ImGuiIO &io = ImGui::GetIO();

	// Full-screen background — palette-aware so light theme doesn't
	// punch a black hole behind the translucent card rows.
	{
		const ATMobilePalette &bgPal = ATMobileGetPalette();
		ImGui::GetBackgroundDrawList()->AddRectFilled(
			ImVec2(0, 0), io.DisplaySize, bgPal.windowBg);
	}

	float insetT = (float)mobileState.layout.insets.top;
	float insetB = (float)mobileState.layout.insets.bottom;
	float insetL = (float)mobileState.layout.insets.left;
	float insetR = (float)mobileState.layout.insets.right;

	ImGui::SetNextWindowPos(ImVec2(insetL, insetT));
	ImGui::SetNextWindowSize(ImVec2(
		io.DisplaySize.x - insetL - insetR,
		io.DisplaySize.y - insetT - insetB));

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
		| ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoBackground;

	if (ImGui::Begin("##MobileDiskMgr", nullptr, flags)) {
		// ESC / B-button / Backspace returns to hamburger.
		if (!s_confirmActive && !s_infoModalOpen) {
			bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
			if (!ImGui::IsAnyItemActive()) {
				back = back
					|| ImGui::IsKeyPressed(ImGuiKey_Escape, false)
					|| ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
			}
			if (back)
				mobileState.currentScreen = ATMobileUIScreen::HamburgerMenu;
		}

		// 8 dp top padding so the header never sits flush with the
		// status bar on devices with a small top inset.
		ImGui::Dummy(ImVec2(0, dp(8.0f)));

		// Header
		float headerH = dp(48.0f);
		if (ATTouchButton("<", ImVec2(dp(48.0f), headerH),
			ATTouchButtonStyle::Subtle))
		{
			mobileState.currentScreen = ATMobileUIScreen::HamburgerMenu;
		}
		ImGui::SameLine();
		ImGui::SetCursorPosY(
			ImGui::GetCursorPosY() + (headerH - ImGui::GetTextLineHeight()) * 0.5f);
		ImGui::SetWindowFontScale(1.15f);
		{
			const ATMobilePalette &hdrPal = ATMobileGetPalette();
			ImGui::TextColored(ATMobileCol(hdrPal.text), "Disk Drives");
		}
		ImGui::SetWindowFontScale(1.0f);

		ImGui::Separator();
		ImGui::Spacing();

		// Single-scroll layout: the whole window scrolls — drive list
		// AND emulation-level footer share one scroll region.  This
		// avoids the previous "reserve 140 dp for footer, clip
		// everything else" split, which clipped content on short
		// viewports.
		ATTouchDragScroll();

		// Default: D1:-D4: (the 99% case)
		int visibleDrives = s_mobileShowAllDrives ? 15 : 4;
		for (int i = 0; i < visibleDrives; ++i)
			RenderMobileDiskRow(sim, i, mobileState);

		// Show/hide additional drives
		ImGui::Spacing();
		if (ATTouchButton(
			s_mobileShowAllDrives ? "Hide drives D5:-D15:" : "Show drives D5:-D15:",
			ImVec2(-1, dp(48.0f))))
		{
			s_mobileShowAllDrives = !s_mobileShowAllDrives;
		}

		// One saving type governs every disk drive in Gaming Mode.  The
		// choice is persisted as the normal Altirra media default and is
		// also applied immediately to all 15 drive interfaces.
		ImGui::Spacing();
		ATTouchSection("Saving Type");
		extern ATOptions g_ATOptions;
		int writeModeIdx = GetMobileWriteModeIndex(
			g_ATOptions.mDefaultWriteMode);
		if (ATTouchSegmented("All disk drives", &writeModeIdx,
			kMobileWriteModeLabels,
			(int)(sizeof(kMobileWriteModeLabels)
				/ sizeof(kMobileWriteModeLabels[0]))))
		{
			SetMobileGlobalWriteMode(sim,
				kMobileWriteModeValues[writeModeIdx]);
		}

		switch (g_ATOptions.mDefaultWriteMode) {
		case kATMediaWriteMode_RO:
			ATTouchMutedText(
				"Games see every disk as write-protected. Use this for "
				"demos or a guaranteed clean start.");
			break;

		case kATMediaWriteMode_VRWSafe:
			ATTouchMutedText(
				"Games can save while you play, but changes are discarded "
				"when the disk is ejected or Altirra closes.");
			break;

		case kATMediaWriteMode_VRW:
			ATTouchMutedText(
				"Temporary saving, with disk formatting allowed. Changes are "
				"discarded when the disk is ejected or Altirra closes.");
			break;

		case kATMediaWriteMode_RW:
			ATTouchMutedText(
				"Saves game progress and high scores between sessions. "
				"Altirra keeps a protected local copy; the source disk image "
				"is not changed.");
			break;
		}
		ATTouchMutedText(
			"Changes apply to all drives now and become the default for "
			"disks mounted later.");

		// Footer: global emulation-level segmented control
		ImGui::Spacing();
		ATTouchSection("Emulation Level");

		// Match the desktop ui_disk.cpp ordering but collapse to the
		// handful of options a mobile user actually cares about.
		static const ATDiskEmulationMode kMobileEmuValues[] = {
			kATDiskEmulationMode_Generic,
			kATDiskEmulationMode_FastestPossible,
			kATDiskEmulationMode_810,
			kATDiskEmulationMode_1050,
			kATDiskEmulationMode_Happy1050,
		};
		static const char *kMobileEmuLabels[] = {
			"Generic", "Fast", "810", "1050", "Happy",
		};
		constexpr int kNumMobileEmu =
			sizeof(kMobileEmuValues) / sizeof(kMobileEmuValues[0]);

		ATDiskEmulationMode curEmu = sim.GetDiskDrive(0).GetEmulationMode();
		int emuIdx = 0;
		for (int i = 0; i < kNumMobileEmu; ++i)
			if (kMobileEmuValues[i] == curEmu) { emuIdx = i; break; }

		if (ATTouchSegmented("Drive type", &emuIdx,
			kMobileEmuLabels, kNumMobileEmu))
		{
			for (int i = 0; i < 15; ++i)
				sim.GetDiskDrive(i).SetEmulationMode(kMobileEmuValues[emuIdx]);
		}

		// 8 dp bottom padding so the segmented control never sits
		// flush with the gesture-bar inset on tight viewports.
		ImGui::Dummy(ImVec2(0, dp(8.0f)));

		ATTouchEndDragScroll();
	}
	ImGui::End();
}
