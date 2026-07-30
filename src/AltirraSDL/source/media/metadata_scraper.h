//	AltirraSDL - Game Library metadata scraper engine
//
//	Owns the worker threads, the work queue, rate limiting and media
//	downloads for the online metadata feature.
//
//	Threading contract, mirroring ATGameLibrary::ScanThread and
//	ATNetplayUI::LobbyWorker:
//
//	  - Start() snapshots everything the workers need (paths, sizes,
//	    CRCs, names) on the calling thread and returns immediately.
//	    Workers never dereference a GameEntry, so a concurrent rescan
//	    cannot pull the ground out from under them.
//	  - Progress is exposed through atomics; strings through a mutex.
//	  - ConsumeResults() runs on the MAIN thread and is the only place
//	    that writes back into the library.  Call it once per frame from
//	    wherever ConsumeScanResults() is already called.
//	  - Cancel() is safe from any thread and must be called on every
//	    exit path (window close, screen change, app suspend, shutdown).
//
//	Deliberately NOT built on ui_progress.cpp: that file's own header
//	explains that a foreground ATProgress scope cannot animate under
//	ImGui, because a synchronous main-thread task never yields a
//	NewFrame/Render/Present cycle.  A scrape is long, cancellable and
//	backgroundable, so it polls instead.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <vd2/system/VDString.h>

#include "../ui/gamelibrary/game_library.h"
#include "metadata_settings.h"

class GameArtCache;

class ATMetadataScraper {
public:
	ATMetadataScraper() = default;
	~ATMetadataScraper();

	// Begins a run over the given entry indices — build them with
	// ATMetadataSelectEntries, which is where the "only missing" and
	// "don't touch my edits" filtering lives.  No-op when a run is
	// already in flight.  Returns false when the run could not start
	// (no HTTPS backend, no developer credential, empty selection) and
	// sets the banner to an explanation.
	bool Start(ATGameLibrary& lib, std::vector<int> entryIndices);

	// Requests cancellation and returns immediately.  Already-written
	// files and already-finished entries are kept: a cancelled run is a
	// shorter run, never a corrupt one.
	void Cancel();

	// Cancel + join.  Call from shutdown paths.
	void Shutdown();

	bool IsRunning() const {
		return mRunning.load(std::memory_order_acquire);
	}

	int GetTotal()    const { return mTotal.load(std::memory_order_relaxed); }
	int GetDone()     const { return mDone.load(std::memory_order_relaxed); }
	int GetMatched()  const { return mMatched.load(std::memory_order_relaxed); }
	int GetNotFound() const { return mNotFound.load(std::memory_order_relaxed); }
	int GetErrors()   const { return mErrors.load(std::memory_order_relaxed); }

	// Name of a game currently being fetched, for the status line.
	VDStringA GetCurrentName() const;

	// How a finished run should be reported to the user.
	enum class RunReport {
		None,
		Success,   // everything asked for came back
		Warning,   // finished, but some or all games are simply not there
		Failure,   // could not finish: network, quota, closed API
	};

	// MAIN THREAD ONLY.  True exactly once after a run ends, with a
	// sentence fit to show the user verbatim.
	//
	// This exists because "nothing happened" is the worst possible
	// answer to a download the user explicitly asked for: without it
	// they cannot tell a game that is genuinely absent from the database
	// (retrying will never help) from a server that was briefly
	// unreachable (retrying is exactly right), so they retry blindly.
	bool ConsumeRunReport(VDStringA& outText, RunReport& outKind);

	// Sticky, user-facing explanation of why a run stopped early
	// (quota, API closed, no credential).  Empty when there is none.
	VDStringA GetBanner() const;
	void SetBanner(const char *text);
	void ClearBanner();

	// MAIN THREAD ONLY.  Applies finished results into the library,
	// invalidates stale art thumbnails, and saves the cache when
	// anything changed.  Returns true when the library was modified.
	bool ConsumeResults(ATGameLibrary& lib, GameArtCache *artCache);

private:
	// Immutable snapshot of one unit of work.
	struct Job {
		int       mEntryIndex = -1;
		VDStringW mVariantPath;
		VDStringA mRomName;
		VDStringA mDisplayName;
		uint64_t  mFileSize = 0;
		uint32_t  mCRC32 = 0;          // 0 => worker computes it
		int       mSystemId = 0;
		int       mAltSystemId = 0;    // 0 => no second system to try
		uint32_t  mPinnedGameId = 0;
	};

	struct Result {
		int          mEntryIndex = -1;
		// Stable identity of the game this result belongs to.  The
		// index alone is not enough: a rescan finishing mid-run
		// reorders mEntries, and applying by index would then write
		// one game's cover onto another.
		VDStringW    mVariantPath;
		uint32_t     mComputedCRC32 = 0;
		GameMetadata mMeta;
		bool         mbHaveMeta = false;
		// Media files whose bytes were rewritten in place, so the main
		// thread can drop their stale scaled thumbnails.
		std::vector<VDStringW> mInvalidatePaths;
	};

	void ControllerThread();
	void WorkerLoop();
	bool ProcessJob(const Job& job, Result& result);

	// Downloads one media URL into {configDir}/media/, returning the
	// path relative to the config dir.  Empty on failure — a missing
	// cover is never a reason to fail the whole entry.
	VDStringW DownloadMedia(const std::string& url, uint32_t crc32,
		const char *slotName, std::vector<VDStringW>& invalidatePaths);

	void SetCurrentName(const VDStringA& name);
	void BuildRunReport();

	std::thread              mController;
	std::vector<std::thread> mWorkers;

	// Set by the run thread as it exits; drained by ConsumeRunReport.
	std::atomic<bool> mReportPending{false};

	std::atomic<bool> mRunning{false};
	std::atomic<bool> mCancel{false};
	std::atomic<bool> mFinished{false};
	std::atomic<int>  mTotal{0};
	std::atomic<int>  mDone{0};
	std::atomic<int>  mMatched{0};
	std::atomic<int>  mNotFound{0};
	std::atomic<int>  mErrors{0};
	// Set when a worker hits a run-ending condition; every other worker
	// notices and stops pulling work.
	std::atomic<bool> mAbort{false};

	// Milliseconds each worker waits between requests.  Set once by the
	// controller from the account's allowance.
	std::atomic<int>  mSpacingMs{1500};

	mutable std::mutex mStateMutex;
	VDStringA          mCurrentName;
	VDStringA          mBanner;
	// Built once when the run ends, under mStateMutex.
	VDStringA          mReportText;
	int                mReportKind = 0;
	// The single game a one-entry run was for, so the report can name
	// it instead of saying "1 game".
	VDStringA          mSingleName;

	std::mutex        mQueueMutex;
	std::vector<Job>  mJobs;
	size_t            mNextJob = 0;

	std::mutex           mResultMutex;
	std::vector<Result>  mResults;

	// Snapshotted at Start() so workers never touch the library, and
	// never read settings the UI thread may be editing concurrently.
	// A run also keeps the shape it was started with rather than
	// changing halfway through.
	VDStringA          mConfigDir;
	VDStringA          mMediaDir;
	ATMetadataSettings mSettings;
};

// Process-wide instance, shared by both frontends.
ATMetadataScraper& ATMetadataGetScraper();

// Build the list of entry indices a run should cover.
//   onlyMissing = true  -> entries that have never matched
//   onlyMissing = false -> every entry, except user-edited ones unless
//                          the user opted into overwriting them
std::vector<int> ATMetadataSelectEntries(const ATGameLibrary& lib,
	bool onlyMissing);

// How many entries a run over `onlyMissing` would cover.  Used for the
// button subtitles so the user knows the cost before pressing.
int ATMetadataCountEntries(const ATGameLibrary& lib, bool onlyMissing);
