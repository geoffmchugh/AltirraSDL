//	AltirraSDL - Game metadata UI, shared between both frontends
//
//	The Desktop dialog and the Gaming Mode page present the same feature
//	with different chrome, so everything that is not pure layout lives
//	here: the account test, the explainer copy, and the actions.  Keeping
//	one copy is what stops the two frontends drifting apart.

#pragma once

#include <vd2/system/VDString.h>

class ATGameLibrary;
struct GameEntry;
enum class GameMetaStatus : unsigned char;
enum class ATMetadataAccessMode : int;

// --- Shared fact lines -----------------------------------------------
//
// Three presentations show the same handful of facts about a game — the
// Desktop details pane, the Gaming Mode preview panel, and the Gaming
// Mode full-screen details sheet.  Building the strings once here is
// what stops them drifting apart (and what stops one of them printing
// a stray separator around an absent field).
//
// Each returns an empty string when it has nothing to say, so callers
// can simply skip the line rather than reserving space for it.
//
//   Title  : provider title if we have one, else the scanned name
//   Facts  : "Atari  ·  1983  ·  17/20"
//   Genre  : "Shoot'em up  ·  up to 2 players"
//   Copy   : "Disk  ·  3 variants  ·  played 12 times"
VDStringA ATUIMetadataDisplayTitle(const GameEntry& entry);
VDStringA ATUIMetadataFactsLine(const GameEntry& entry);
VDStringA ATUIMetadataGenreLine(const GameEntry& entry);
VDStringA ATUIMetadataCopyLine(const GameEntry& entry);

// One-line "why there is nothing to show" text, or empty when the entry
// does have metadata.  Shared so both frontends say the same thing.
const char *ATUIMetadataEmptyReason(const GameEntry& entry);

// --- Shared copy -----------------------------------------------------
//
// One string, rendered by two frontends (a Desktop collapsing header and
// a Gaming Mode info modal).

extern const char *const kATMetadataAccountHelpTitle;
extern const char *const kATMetadataAccountHelpBody;
extern const char *const kATMetadataProviderUrl;

// --- Account test ----------------------------------------------------
//
// Runs on a worker thread so a slow or unreachable provider never
// freezes a frame.  Poll from the main thread each frame.

enum class ATMetadataTestState {
	Idle,
	Running,
	Success,
	Failure,
};

void ATUIMetadataStartAccountTest();
ATMetadataTestState ATUIMetadataGetTestState();
// One-line result, e.g. "Level 3 - 4 threads - 312 / 20000 today".
VDStringA ATUIMetadataGetTestMessage();
// Main thread, once per frame: reaps the worker when it finishes.
void ATUIMetadataPumpAccountTest();
// Cancel + join.  Must be called from every shutdown path.
void ATUIMetadataShutdownAccountTest();

// --- Shared actions --------------------------------------------------

// Apply and persist the network policy. Switching to Disabled also cancels
// scraper work and any account test already in flight.
void ATUIMetadataSetAccessMode(ATMetadataAccessMode mode);

// Starts a run, wiring up the banner on failure.  `onlyMissing` false
// means "everything", subject to the overwrite-user-edits setting.
void ATUIMetadataStartDownload(ATGameLibrary& lib, bool onlyMissing);

// Starts a run over exactly one entry, ignoring "only missing".
void ATUIMetadataStartDownloadForEntry(ATGameLibrary& lib, int entryIndex);

// Clears an entry's metadata and deletes the media files it owned.
void ATUIMetadataClearEntry(ATGameLibrary& lib, int entryIndex);

// Deletes every downloaded media file and clears every entry's
// metadata.  Destructive: callers must confirm first.
void ATUIMetadataClearAll(ATGameLibrary& lib);

// Deletes downloaded media files but keeps the text metadata.
void ATUIMetadataDeleteAllMedia(ATGameLibrary& lib);

// Total bytes and file count under {configDir}/media.
void ATUIMetadataGetStorageUsage(const ATGameLibrary& lib,
	uint64_t& outBytes, int& outFiles);

// True when the feature can actually run right now (HTTPS backend
// present and a developer credential available).  `outWhyNot` receives
// a user-facing explanation when it returns false.
bool ATUIMetadataIsUsable(VDStringA& outWhyNot);

// True when a download for one entry can be *started* right now — i.e.
// usable AND no run already in flight.
//
// Prefer this over `IsUsable(w) && !IsRunning()` at call sites that
// disable a control: that idiom leaves `outWhyNot` empty in the
// already-running case, which is how you end up with a greyed-out
// button and no explanation.  This one always fills `outWhyNot` when it
// returns false.
bool ATUIMetadataCanDownloadNow(VDStringA& outWhyNot);

// MAIN THREAD, once per frame, from wherever the frontend is guaranteed
// to be rendering — not from a screen the user might have navigated away
// from.  Drains the scraper's end-of-run report and turns it into
// whatever this build's feedback surface is (a toast in Gaming Mode; the
// Desktop dialog shows it inline in its progress strip instead).
//
// Without this, a download the user explicitly asked for could finish
// with no visible outcome at all, which is the one thing that makes
// people press the button again and again.
void ATUIMetadataPumpRunFeedback();

// MAIN THREAD, once per frame, alongside ATUIMetadataPumpRunFeedback.
// Starts a background fetch for games that have just appeared in the
// library, when the user has left that switched on.
//
// Consumes GameEntry::mbNewlyAdded whether or not it starts anything, so
// a library that is too large to auto-fetch is considered exactly once
// rather than re-examined on every frame forever.
void ATUIMetadataPumpAutoFetch(ATGameLibrary& lib);

// MAIN THREAD, once per frame.  Applies whatever the scraper's workers
// finished into the library.
//
// This has to run from a screen-independent place.  It used to be called
// only from the Game Browser's render, which meant a download started
// from the Game Details sheet produced nothing visible until the user
// navigated back — the results were sitting in the queue with nobody to
// apply them.
void ATUIMetadataPumpResults(ATGameLibrary& lib);

// The last finished run's message, for a frontend that shows it inline
// rather than as a transient toast.  Empty once dismissed.
const VDStringA& ATUIMetadataGetLastRunText();
int  ATUIMetadataGetLastRunKind();   // 0 none, 1 ok, 2 warning, 3 failure
void ATUIMetadataClearLastRun();

// Short status glyph + colour for a library row / tile badge.
const char *ATUIMetadataStatusGlyph(GameMetaStatus status);
const char *ATUIMetadataStatusLabel(GameMetaStatus status);

// --- Desktop-mode renderers ------------------------------------------
//
// Called from ui_game_library.cpp; defined in ui_game_metadata.cpp.

// The "Metadata" tab body (account, what to download, preferences,
// storage).
void ATUIRenderMetadataTab(ATGameLibrary& lib);

// The Games-tab toolbar: split Download button + counts.  Returns the
// height it consumed so the caller can size the table.
void ATUIRenderMetadataToolbar(ATGameLibrary& lib, int selectedEntry);

// Context-menu items for one library row.  Call inside a BeginPopup.
void ATUIRenderMetadataRowMenu(ATGameLibrary& lib, int entryIndex);

// Non-modal progress strip, drawn at the bottom of the dialog while a
// run is in flight (or a banner is pending).  Returns true when it drew
// something, so the caller can reserve space for it.
bool ATUIRenderMetadataProgress(ATGameLibrary& lib);

// Right-hand details pane for the selected entry.  Draws its own
// scrolling region and a pinned action footer, so the caller only has
// to give it a rectangle (via BeginChild) and a selection.
//
// `onLaunch` fires when the user presses the pane's Play button; it is
// a callback rather than a return value because the pane also has to
// keep drawing after the launch is dispatched.
void ATUIRenderMetadataDetails(ATGameLibrary& lib, int entryIndex,
	void (*onLaunch)(void *userData, int entryIndex) = nullptr,
	void *userData = nullptr);

// --- Global artwork preference ---------------------------------------
//
// Which kind of downloaded image the library shows — everywhere at once.
// The left/right switch in either details panel drives this, so one
// press answers "show me screenshots" for the whole library rather than
// for one game.  Persisted immediately; see
// ATMetadataSettings::mArtSlot.

// Step the preference by +1 / -1, wrapping, and persist it.
void ATUIMetadataCycleArtSlot(int delta);

// Name of the current preference, e.g. "Screenshot".
const char *ATUIMetadataArtSlotName();

// Caption for one entry's artwork: the preference, plus a note when this
// particular game does not have that kind and a fallback is on screen.
// Saying so is the difference between "the switch is broken" and "this
// game has no cover scan".
VDStringA ATUIMetadataArtCaption(const GameEntry& entry);

// Confirm-and-remove modal for one library entry, shared by the Desktop
// row menu and details pane.  Opening only arms it; the removal happens
// when the user confirms.
//
// Takes the library because it records the entry's variant path, not its
// index — a background scan landing mid-dialog would otherwise retarget
// the confirmation at whatever game inherited that slot.
void ATUIMetadataOpenRemove(ATGameLibrary& lib, int entryIndex);
void ATUIRenderMetadataRemoveConfirm(ATGameLibrary& lib);
