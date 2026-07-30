//	AltirraSDL - Game Library metadata scraper engine
//	See metadata_scraper.h for the threading contract.

#include <stdafx.h>

#include "metadata_scraper.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <SDL3/SDL.h>

#include <vd2/system/file.h>
#include <vd2/system/text.h>

#include "http_client.h"
#include "metadata_screenscraper.h"
#include "metadata_settings.h"
#include "../ui/gamelibrary/game_library_art.h"

namespace {

// Hard ceiling on parallel requests regardless of what the account
// allows.  Four is plenty to saturate a home connection with small JSON
// replies, and keeps us a polite client.
const int kMaxWorkers = 4;

// Anonymous requests are charged to the application's shared developer
// credential, so they get one worker and a wide gap.  A user account
// pays its own way and gets the account's allowance.
const int kAnonymousSpacingMs = 1500;
const int kAccountSpacingMs   = 400;

// Client-side cap on a single anonymous run.  The shared credential is
// the one resource a single enthusiastic user could ruin for everybody,
// so an anonymous run stops here and tells the user how to lift it.
const int kAnonymousRunCap = 50;

void SleepMs(int ms) {
	if (ms > 0)
		SDL_Delay((Uint32)ms);
}

// UTF-8 file name (with extension) of a variant path, which is what
// ScreenScraper matches `romnom` against.
VDStringA BaseNameUtf8(const VDStringW& path) {
	const wchar_t *p = path.c_str();
	const wchar_t *base = p;
	for (const wchar_t *q = p; *q; ++q) {
		if (*q == L'/' || *q == L'\\')
			base = q + 1;
	}
	// Archive members arrive as zip://outer.zip!inner.atr — the inner
	// name is what the provider knows.
	for (const wchar_t *q = base; *q; ++q) {
		if (*q == L'!')
			base = q + 1;
	}
	return VDTextWToU8(VDStringW(base));
}

// ScreenScraper platform for a variant.  Cartridges may be either an
// Atari 8-bit or a 5200 title, so those get a second system to try.
void ChooseSystems(GameMediaType type, bool allow5200, int& primary,
	int& alternate)
{
	primary = kATScreenScraperSystemAtari8Bit;
	alternate = 0;

	// Only cartridge images are ambiguous: 5200 software ships as .car
	// / .bin / .a52, while disks, tapes and executables are 8-bit only.
	if (allow5200 && type == GameMediaType::Cartridge)
		alternate = kATScreenScraperSystemAtari5200;
}

const char *ExtensionForContentType(const char *contentType,
	const std::string& url)
{
	if (contentType) {
		if (strstr(contentType, "png"))  return "png";
		if (strstr(contentType, "jpeg")) return "jpg";
		if (strstr(contentType, "jpg"))  return "jpg";
		if (strstr(contentType, "gif"))  return "gif";
		if (strstr(contentType, "webp")) return "webp";
	}

	// Fall back to whatever the URL claims.  Returning a string literal
	// rather than the parsed extension keeps the lifetime valid; the
	// earlier version collapsed every recognised extension to "png",
	// which meant a JPEG cover was written as cover.png.
	const size_t q = url.find('?');
	const std::string path = (q == std::string::npos) ? url : url.substr(0, q);
	const size_t dot = path.rfind('.');
	if (dot != std::string::npos && path.size() - dot <= 6) {
		const std::string ext = path.substr(dot + 1);
		if (ext == "png")                    return "png";
		if (ext == "jpg" || ext == "jpeg")   return "jpg";
		if (ext == "gif")                    return "gif";
		if (ext == "webp")                   return "webp";
	}
	return "png";
}

// Stable 32-bit hash of a path, used to name media files for entries
// whose CRC32 could not be computed (unreadable file, archive member
// that failed to open).  Without it every such entry would write to
// "00000000-box.png" and silently overwrite the previous one.
uint32_t HashPath(const VDStringW& path) {
	// FNV-1a over the UTF-8 bytes: deterministic across runs and
	// platforms, which is what makes the file name stable.
	const VDStringA utf8 = VDTextWToU8(path);
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < utf8.size(); ++i) {
		h ^= (unsigned char)utf8[i];
		h *= 16777619u;
	}
	// Never collide with a real CRC of 0 meaning "unknown".
	return h ? h : 1u;
}

bool WriteFileBytes(const VDStringW& path, const std::vector<uint8_t>& bytes) {
	try {
		VDFileStream fs(path.c_str(),
			nsVDFile::kWrite | nsVDFile::kDenyAll | nsVDFile::kCreateAlways);
		if (!bytes.empty())
			fs.Write(bytes.data(), (sint32)bytes.size());
		return true;
	} catch (...) {
		return false;
	}
}

ATMetadataScraper g_scraper;

}  // namespace

ATMetadataScraper& ATMetadataGetScraper() {
	return g_scraper;
}

// ---------------------------------------------------------------------------
// Selection helpers
// ---------------------------------------------------------------------------

std::vector<int> ATMetadataSelectEntries(const ATGameLibrary& lib,
	bool onlyMissing)
{
	const auto& entries = lib.GetEntries();

	std::vector<int> out;
	out.reserve(entries.size());

	for (size_t i = 0; i < entries.size(); ++i) {
		const GameEntry& e = entries[i];
		if (e.mVariants.empty())
			continue;

		// UserEdited is legacy — see game_library.h.  It means "this
		// entry has metadata", so it behaves exactly like Matched here.
		if (onlyMissing
			&& (e.mMeta.mStatus == GameMetaStatus::Matched
				|| e.mMeta.mStatus == GameMetaStatus::UserEdited))
		{
			continue;
		}

		out.push_back((int)i);
	}

	return out;
}

int ATMetadataCountEntries(const ATGameLibrary& lib, bool onlyMissing) {
	// Counted in place rather than via ATMetadataSelectEntries: this is
	// called every frame by the Gaming Mode settings rows, the browser's
	// first-run nudge and the Desktop toolbar, and building a throwaway
	// vector of every library index on each of those frames is a real
	// cost on a large library.
	const auto& entries = lib.GetEntries();

	int count = 0;
	for (const GameEntry& e : entries) {
		if (e.mVariants.empty())
			continue;
		if (onlyMissing
			&& (e.mMeta.mStatus == GameMetaStatus::Matched
				|| e.mMeta.mStatus == GameMetaStatus::UserEdited))
		{
			continue;
		}
		++count;
	}
	return count;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

ATMetadataScraper::~ATMetadataScraper() {
	Shutdown();
}

bool ATMetadataScraper::Start(ATGameLibrary& lib,
	std::vector<int> entryIndices)
{
	if (mRunning.load(std::memory_order_acquire))
		return false;

	// Join a previous run's threads before reusing the members.
	if (mController.joinable())
		mController.join();
	mWorkers.clear();

	if (!ATHttp::Available()) {
		SetBanner(ATScreenScraperOutcomeText(
			ATScreenScraperOutcome::Unavailable));
		return false;
	}
	if (!ATMetadataHaveDevCredential()) {
		SetBanner(ATScreenScraperOutcomeText(
			ATScreenScraperOutcome::NotConfigured));
		return false;
	}
	if (entryIndices.empty()) {
		SetBanner("Nothing to download \xE2\x80\x94 every game already "
			"has metadata.");
		return false;
	}

	const ATMetadataSettings& settings = ATMetadataGetSettings();
	const bool anonymous = !(settings.mbUseUserAccount
		&& !settings.mUserName.empty() && !settings.mUserPassword.empty());

	// Build the immutable job list on this (main) thread.  After this
	// point no worker touches the library.
	std::vector<Job> jobs;
	jobs.reserve(entryIndices.size());

	const auto& entries = lib.GetEntries();
	for (int index : entryIndices) {
		if (index < 0 || (size_t)index >= entries.size())
			continue;
		const GameEntry& e = entries[index];
		if (e.mVariants.empty())
			continue;

		const GameVariant& v = e.mVariants[0];

		Job job;
		job.mEntryIndex   = index;
		job.mVariantPath  = v.mPath;
		job.mRomName      = BaseNameUtf8(v.mPath);
		job.mDisplayName  = VDTextWToU8(e.mDisplayName);
		job.mFileSize     = v.mFileSize;
		job.mCRC32        = v.mGameFileCRC32;
		job.mPinnedGameId = e.mMeta.mProviderGameId;
		ChooseSystems(v.mType, settings.mbTry5200Fallback,
			job.mSystemId, job.mAltSystemId);

		jobs.push_back(std::move(job));
	}

	// A one-game run gets to name the game in its report.
	{
		std::lock_guard<std::mutex> lock(mStateMutex);
		mSingleName = (jobs.size() == 1) ? jobs[0].mDisplayName : VDStringA();
		mReportText.clear();
		mReportKind = 0;
	}
	mReportPending.store(false, std::memory_order_release);

	if (jobs.empty()) {
		SetBanner("Nothing to download.");
		return false;
	}

	bool cappedAnonymous = false;
	if (anonymous && (int)jobs.size() > kAnonymousRunCap) {
		jobs.resize(kAnonymousRunCap);
		cappedAnonymous = true;
	}

	if (lib.GetConfigDir().empty()) {
		SetBanner("The game library is not ready yet. Try again in a moment.");
		return false;
	}

	// Snapshot before any thread starts; see the member comment.
	mSettings = settings;

	mConfigDir = lib.GetConfigDir();
	mMediaDir = mConfigDir;
	if (!mMediaDir.empty() && mMediaDir.back() != '/')
		mMediaDir += '/';
	mMediaDir += "media";
	SDL_CreateDirectory(mMediaDir.c_str());

	{
		std::lock_guard<std::mutex> lock(mQueueMutex);
		mJobs = std::move(jobs);
		mNextJob = 0;
	}
	{
		std::lock_guard<std::mutex> lock(mResultMutex);
		mResults.clear();
	}

	mTotal.store((int)mJobs.size(), std::memory_order_relaxed);
	mDone.store(0, std::memory_order_relaxed);
	mMatched.store(0, std::memory_order_relaxed);
	mNotFound.store(0, std::memory_order_relaxed);
	mErrors.store(0, std::memory_order_relaxed);
	mCancel.store(false, std::memory_order_relaxed);
	mAbort.store(false, std::memory_order_relaxed);
	mFinished.store(false, std::memory_order_relaxed);
	mSpacingMs.store(anonymous ? kAnonymousSpacingMs : kAccountSpacingMs,
		std::memory_order_relaxed);
	SetCurrentName(VDStringA());

	ClearBanner();
	if (cappedAnonymous) {
		SetBanner("Downloading the first 50 games. Anonymous downloads "
			"share one allowance with every AltirraSDL user \xE2\x80\x94 "
			"add a free ScreenScraper account to do the whole library.");
	}

	mRunning.store(true, std::memory_order_release);
	mController = std::thread([this] { ControllerThread(); });
	return true;
}

void ATMetadataScraper::Cancel() {
	mCancel.store(true, std::memory_order_release);
}

void ATMetadataScraper::Shutdown() {
	Cancel();
	if (mController.joinable())
		mController.join();
	mWorkers.clear();
	mRunning.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Status accessors
// ---------------------------------------------------------------------------

VDStringA ATMetadataScraper::GetCurrentName() const {
	std::lock_guard<std::mutex> lock(mStateMutex);
	return mCurrentName;
}

void ATMetadataScraper::SetCurrentName(const VDStringA& name) {
	std::lock_guard<std::mutex> lock(mStateMutex);
	mCurrentName = name;
}

VDStringA ATMetadataScraper::GetBanner() const {
	std::lock_guard<std::mutex> lock(mStateMutex);
	return mBanner;
}

void ATMetadataScraper::SetBanner(const char *text) {
	std::lock_guard<std::mutex> lock(mStateMutex);
	mBanner = text ? text : "";
}

void ATMetadataScraper::ClearBanner() {
	std::lock_guard<std::mutex> lock(mStateMutex);
	mBanner.clear();
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

void ATMetadataScraper::ControllerThread() {
	const ATMetadataSettings& settings = mSettings;
	const bool anonymous = !(settings.mbUseUserAccount
		&& !settings.mUserName.empty() && !settings.mUserPassword.empty());

	int workerCount = 1;

	if (!anonymous && !mCancel.load(std::memory_order_acquire)) {
		// Ask the provider how much parallelism this account is allowed
		// rather than guessing.  A failure here is not fatal: we simply
		// stay conservative and let the per-request error handling deal
		// with whatever comes back.
		ATScreenScraperAccount account;
		if (ATScreenScraperFetchAccount(settings, &mCancel, account)
			&& account.mbValid)
		{
			workerCount = account.mMaxThreads > 0 ? account.mMaxThreads : 1;
			if (workerCount > kMaxWorkers)
				workerCount = kMaxWorkers;

			if (account.mMaxRequestsPerDay > 0
				&& account.mRequestsToday >= account.mMaxRequestsPerDay)
			{
				SetBanner(ATScreenScraperOutcomeText(
					ATScreenScraperOutcome::QuotaExceeded));
				mAbort.store(true, std::memory_order_release);
			}
		}
	}

	if (workerCount < 1)
		workerCount = 1;

	if (!mAbort.load(std::memory_order_acquire)) {
		mWorkers.reserve((size_t)workerCount);
		for (int i = 0; i < workerCount; ++i)
			mWorkers.emplace_back([this] { WorkerLoop(); });

		for (auto& t : mWorkers) {
			if (t.joinable())
				t.join();
		}
	}

	mWorkers.clear();
	SetCurrentName(VDStringA());
	BuildRunReport();
	mFinished.store(true, std::memory_order_release);
	mRunning.store(false, std::memory_order_release);
}

// Turn the run's counters into one sentence the user can act on.  The
// distinction that matters is permanent vs temporary: "this game is not
// in the database" and "the server did not answer" look identical from
// the outside, and only one of them is worth retrying.
void ATMetadataScraper::BuildRunReport() {
	const int total    = mTotal.load(std::memory_order_relaxed);
	const int matched  = mMatched.load(std::memory_order_relaxed);
	const int notFound = mNotFound.load(std::memory_order_relaxed);
	const int errors   = mErrors.load(std::memory_order_relaxed);
	const bool cancelled = mCancel.load(std::memory_order_acquire);

	VDStringA banner;
	VDStringA single;
	{
		std::lock_guard<std::mutex> lock(mStateMutex);
		banner = mBanner;
		single = mSingleName;
	}

	VDStringA text;
	RunReport kind = RunReport::Success;
	char buf[256];

	if (cancelled) {
		snprintf(buf, sizeof buf, "Download cancelled - %d of %d done.",
			matched, total);
		text = buf;
		kind = RunReport::Warning;
	} else if (!banner.empty()) {
		// A fatal outcome already wrote the precise reason (quota, API
		// closed, blacklisted, no credential); repeating it beats
		// inventing a vaguer one.
		text = banner;
		kind = RunReport::Failure;
	} else if (total == 1) {
		// Single-game runs are the ones started from a details screen,
		// where the user is looking straight at the result.  Name the
		// game and say precisely what happened to it.
		const char *name = single.empty() ? "That game" : single.c_str();
		if (matched) {
			snprintf(buf, sizeof buf, "%s: metadata updated.", name);
			kind = RunReport::Success;
		} else if (notFound) {
			snprintf(buf, sizeof buf,
				"%s: no match in ScreenScraper. The file is not in their "
				"database, so trying again will not help.", name);
			kind = RunReport::Warning;
		} else {
			snprintf(buf, sizeof buf,
				"%s: could not reach ScreenScraper. This is usually "
				"temporary - try again in a minute.", name);
			kind = RunReport::Failure;
		}
		text = buf;
	} else if (errors > 0 && matched == 0) {
		snprintf(buf, sizeof buf,
			"Could not reach ScreenScraper for any of %d games. This is "
			"usually temporary - try again in a minute.", total);
		text = buf;
		kind = RunReport::Failure;
	} else {
		snprintf(buf, sizeof buf,
			"%d of %d games updated - %d not in the database%s.",
			matched, total, notFound,
			errors ? ", some could not be reached" : "");
		text = buf;
		kind = (matched == 0) ? RunReport::Warning : RunReport::Success;
	}

	{
		std::lock_guard<std::mutex> lock(mStateMutex);
		mReportText = text;
		mReportKind = (int)kind;
	}
	mReportPending.store(true, std::memory_order_release);
}

bool ATMetadataScraper::ConsumeRunReport(VDStringA& outText,
	RunReport& outKind)
{
	if (!mReportPending.exchange(false, std::memory_order_acq_rel))
		return false;

	std::lock_guard<std::mutex> lock(mStateMutex);
	outText = mReportText;
	outKind = (RunReport)mReportKind;
	return true;
}

void ATMetadataScraper::WorkerLoop() {
	for (;;) {
		if (mCancel.load(std::memory_order_acquire)
			|| mAbort.load(std::memory_order_acquire))
		{
			break;
		}

		Job job;
		{
			std::lock_guard<std::mutex> lock(mQueueMutex);
			if (mNextJob >= mJobs.size())
				break;
			job = mJobs[mNextJob++];
		}

		SetCurrentName(job.mDisplayName);

		Result result;
		const bool keepGoing = ProcessJob(job, result);

		{
			std::lock_guard<std::mutex> lock(mResultMutex);
			mResults.push_back(std::move(result));
		}
		mDone.fetch_add(1, std::memory_order_relaxed);

		if (!keepGoing)
			break;

		// Politeness gap.  Skipped on the last item so a small run does
		// not appear to hang at 100%.
		{
			std::lock_guard<std::mutex> lock(mQueueMutex);
			if (mNextJob >= mJobs.size())
				break;
		}
		SleepMs(mSpacingMs.load(std::memory_order_relaxed));
	}
}

bool ATMetadataScraper::ProcessJob(const Job& job, Result& result) {
	result.mEntryIndex = job.mEntryIndex;
	result.mVariantPath = job.mVariantPath;

	const ATMetadataSettings& settings = mSettings;

	// Fill in the CRC if the library never needed it before.  This is
	// the same value the netplay joiner caches, and the main thread
	// writes it back into the variant so it is only ever computed once.
	uint32_t crc = job.mCRC32;
	uint64_t size = job.mFileSize;
	if (!crc) {
		uint32_t computed = 0;
		uint64_t computedSize = 0;
		if (ATGameComputeFileCRC32(job.mVariantPath, computed, computedSize)) {
			crc = computed;
			result.mComputedCRC32 = computed;
			if (computedSize)
				size = computedSize;
		}
	}

	if (mCancel.load(std::memory_order_acquire))
		return false;

	ATScreenScraperQuery query;
	query.mCRC32        = crc;
	query.mFileSize     = size;
	query.mRomName      = job.mRomName;
	query.mSystemId     = job.mSystemId;
	query.mPinnedGameId = job.mPinnedGameId;

	ATScreenScraperResult fetched;
	VDStringA error;

	// HTTP 429 means "you are querying too fast", and ScreenScraper's
	// documentation is explicit that the remedy is to slow down.  Widen
	// the gap between requests for the rest of the run and retry this
	// one, rather than burning the entry as a failure.  The widened
	// spacing is deliberately not reset afterwards: the server has told
	// us our pace was wrong, and it will still be wrong in a minute.
	auto fetchWithBackoff = [&](void) {
		ATScreenScraperOutcome result = ATScreenScraperFetch(
			query, settings, &mCancel, fetched, error);

		for (int attempt = 0;
			attempt < 3
			&& result == ATScreenScraperOutcome::RateLimited
			&& !mCancel.load(std::memory_order_acquire);
			++attempt)
		{
			int spacing = mSpacingMs.load(std::memory_order_relaxed) * 2;
			if (spacing > 8000)
				spacing = 8000;
			mSpacingMs.store(spacing, std::memory_order_relaxed);

			SleepMs(spacing);
			if (mCancel.load(std::memory_order_acquire))
				break;

			result = ATScreenScraperFetch(query, settings, &mCancel,
				fetched, error);
		}
		return result;
	};

	ATScreenScraperOutcome outcome = fetchWithBackoff();

	// Fallback 1: same system, filename + size only.  Catches re-dumps
	// and cracked images whose bytes differ from the archived set.
	if (outcome == ATScreenScraperOutcome::NotFound
		&& !query.mPinnedGameId && crc
		&& !mCancel.load(std::memory_order_acquire))
	{
		SleepMs(mSpacingMs.load(std::memory_order_relaxed));
		query.mbNameOnly = true;
		outcome = fetchWithBackoff();
	}

	// Fallback 2: the *cleaned* title through the same exact lookup.
	//
	// jeuInfos matches romnom exactly, so "Ballblazer (1984)(Lucasfilm
	// Games)[cr TCS].atr" never matches the database's plain
	// "Ballblazer" — but the decoration-stripped name does, and that one
	// substitution recovers a large share of a typical 8-bit collection
	// at the cost of one extra request.
	const VDStringA cleanName =
		ATScreenScraperCleanName(job.mRomName.c_str());
	if (outcome == ATScreenScraperOutcome::NotFound
		&& !query.mPinnedGameId
		&& !cleanName.empty() && cleanName != job.mRomName
		&& !mCancel.load(std::memory_order_acquire))
	{
		SleepMs(mSpacingMs.load(std::memory_order_relaxed));
		query.mbNameOnly = true;
		query.mRomName = cleanName;
		// Size is meaningless once we are guessing at the title: a
		// cracked image is a different length from the archived dump,
		// and sending it would only narrow away the match we want.
		query.mFileSize = 0;
		outcome = fetchWithBackoff();
	}

	// Fallback 3: the other Atari platform, for ambiguous cartridges.
	if (outcome == ATScreenScraperOutcome::NotFound
		&& !query.mPinnedGameId && job.mAltSystemId
		&& !mCancel.load(std::memory_order_acquire))
	{
		SleepMs(mSpacingMs.load(std::memory_order_relaxed));
		query.mbNameOnly = false;
		query.mRomName = job.mRomName;
		query.mFileSize = job.mFileSize;
		query.mSystemId = job.mAltSystemId;
		outcome = fetchWithBackoff();
	}

	// Fallback 4: a real fuzzy search.
	//
	// Everything above is still an exact database lookup — by hash or by
	// name.  This is the only step that can match a file nobody has ever
	// indexed under that name, which on this platform is most of a
	// hand-curated collection.  It costs two extra requests (search, then
	// fetch by id) and can be wrong, so it is opt-out and every candidate
	// has to clear a similarity threshold before it is accepted.
	if (outcome == ATScreenScraperOutcome::NotFound
		&& settings.mbFuzzyNameMatch
		&& !query.mPinnedGameId
		&& !cleanName.empty()
		&& !mCancel.load(std::memory_order_acquire))
	{
		SleepMs(mSpacingMs.load(std::memory_order_relaxed));

		std::vector<ATScreenScraperCandidate> candidates;
		VDStringA searchError;
		const ATScreenScraperOutcome searchOutcome = ATScreenScraperSearch(
			cleanName, job.mSystemId, settings, &mCancel, candidates,
			searchError);

		if (ATScreenScraperIsFatal(searchOutcome)) {
			// A quota or blacklist answer to the search is just as fatal
			// as one to a fetch; surface it rather than swallowing it.
			outcome = searchOutcome;
			error = searchError;
		} else if (searchOutcome == ATScreenScraperOutcome::Matched
			&& !mCancel.load(std::memory_order_acquire))
		{
			int score = 0;
			const ATScreenScraperCandidate *best =
				ATScreenScraperPickBestCandidate(candidates,
					cleanName.c_str(), score);
			if (best) {
				SleepMs(mSpacingMs.load(std::memory_order_relaxed));
				// Re-fetch by id: the search reply carries only enough to
				// choose, not the media list we actually need.
				query = ATScreenScraperQuery();
				query.mSystemId    = job.mSystemId;
				query.mPinnedGameId = best->mGameId;
				outcome = fetchWithBackoff();
				// The pin is per-request, not persisted.  A fuzzy hit is
				// a guess, and writing it into mProviderGameId would make
				// that guess permanent and unre-checkable on later runs.
			}
		}
	}

	if (mCancel.load(std::memory_order_acquire))
		return false;

	if (ATScreenScraperIsFatal(outcome)) {
		SetBanner(ATScreenScraperOutcomeText(outcome));
		mAbort.store(true, std::memory_order_release);
		mErrors.fetch_add(1, std::memory_order_relaxed);
		result.mMeta.mStatus = GameMetaStatus::Error;
		result.mbHaveMeta = false;
		return false;
	}

	if (outcome == ATScreenScraperOutcome::NotFound) {
		mNotFound.fetch_add(1, std::memory_order_relaxed);
		result.mMeta = GameMetadata();
		result.mMeta.mStatus = GameMetaStatus::NotFound;
		result.mMeta.mProvider = "screenscraper";
		result.mMeta.mFetchedTime = (uint64_t)time(nullptr);
		result.mbHaveMeta = true;
		return true;
	}

	if (outcome != ATScreenScraperOutcome::Matched) {
		if (outcome == ATScreenScraperOutcome::RateLimited && error.empty())
			error = "ScreenScraper is still rate limiting; skipped.";
		mErrors.fetch_add(1, std::memory_order_relaxed);
		result.mMeta.mStatus = GameMetaStatus::Error;
		result.mbHaveMeta = false;
		return true;
	}

	GameMetadata meta = fetched.mMeta;
	meta.mMatchedCRC32 = crc;
	meta.mFetchedTime = (uint64_t)time(nullptr);

	if (!settings.mbDownloadText) {
		meta.mTitle.clear();
		meta.mDescription.clear();
		meta.mPublisher.clear();
		meta.mDeveloper.clear();
		meta.mGenre.clear();
		meta.mYear = 0;
		meta.mPlayersMax = 0;
		meta.mRating = 0;
	}

	// File-name key for this entry's media.  The CRC is preferred
	// because it is stable even if the file moves, but it can be 0 when
	// the game file could not be read — falling back to a path hash
	// keeps those entries from all writing to the same file name.
	const uint32_t mediaKey = crc ? crc : HashPath(job.mVariantPath);

	if (settings.mbDownloadBoxArt && !fetched.mBoxArtUrl.empty()) {
		meta.mBoxArtPath = DownloadMedia(fetched.mBoxArtUrl, mediaKey, "box",
			result.mInvalidatePaths);
	}
	if (settings.mbDownloadTitleShot && !fetched.mTitleShotUrl.empty()) {
		meta.mTitleShotPath = DownloadMedia(fetched.mTitleShotUrl, mediaKey,
			"title", result.mInvalidatePaths);
	}
	if (settings.mbDownloadScreenshot && !fetched.mScreenshotUrl.empty()) {
		meta.mScreenshotPath = DownloadMedia(fetched.mScreenshotUrl, mediaKey,
			"ss", result.mInvalidatePaths);
	}
	if (settings.mbDownloadLogo && !fetched.mLogoUrl.empty()) {
		meta.mLogoPath = DownloadMedia(fetched.mLogoUrl, mediaKey, "wheel",
			result.mInvalidatePaths);
	}

	// A reply with neither usable text nor any media is not a useful
	// match — record it as "not found" so a later run retries instead of
	// treating the entry as done.
	if (!meta.HasAnyText() && !meta.HasAnyMedia()) {
		mNotFound.fetch_add(1, std::memory_order_relaxed);
		meta = GameMetadata();
		meta.mStatus = GameMetaStatus::NotFound;
		meta.mProvider = "screenscraper";
		meta.mFetchedTime = (uint64_t)time(nullptr);
		result.mMeta = std::move(meta);
		result.mbHaveMeta = true;
		return true;
	}

	meta.mStatus = GameMetaStatus::Matched;
	mMatched.fetch_add(1, std::memory_order_relaxed);
	result.mMeta = std::move(meta);
	result.mbHaveMeta = true;
	return true;
}

VDStringW ATMetadataScraper::DownloadMedia(const std::string& url,
	uint32_t crc32, const char *slotName,
	std::vector<VDStringW>& invalidatePaths)
{
	if (url.empty() || mCancel.load(std::memory_order_acquire))
		return VDStringW();

	ATHttp::Request req;
	req.url = url;
	req.timeoutMs = 30000;
	req.cancel = &mCancel;
	req.userAgent = "AltirraSDL/1.0";
	// Cover art is tens to hundreds of KB.  8 MB is generous headroom
	// while still catching a redirect to something unexpected.
	req.maxBodyBytes = 8u * 1024u * 1024u;

	ATHttp::Response resp;
	ATHttp::Get(req, resp);

	if (resp.status != 200 || resp.body.empty())
		return VDStringW();

	// Reject anything that is not an image before it reaches the art
	// cache's decoder — an error page saved as "box.png" would fail to
	// decode on every frame thereafter.
	if (!resp.contentType.empty()
		&& resp.contentType.compare(0, 6, "image/") != 0)
	{
		return VDStringW();
	}

	const char *ext = ExtensionForContentType(resp.contentType.c_str(), url);

	char name[64];
	snprintf(name, sizeof name, "%08X-%s.%s", (unsigned)crc32, slotName, ext);

	VDStringA absolute = mMediaDir;
	absolute += '/';
	absolute += name;

	const VDStringW absoluteW = VDTextU8ToW(absolute);
	if (!WriteFileBytes(absoluteW, resp.body))
		return VDStringW();

	// The bytes behind this path may have changed while the path did
	// not, so the main thread must drop the stale scaled thumbnail.
	invalidatePaths.push_back(absoluteW);

	// Stored relative to the config dir so a moved profile still works.
	VDStringA relative("media/");
	relative += name;
	return VDTextU8ToW(relative);
}

// ---------------------------------------------------------------------------
// Main-thread result application
// ---------------------------------------------------------------------------

bool ATMetadataScraper::ConsumeResults(ATGameLibrary& lib,
	GameArtCache *artCache)
{
	std::vector<Result> results;
	{
		std::lock_guard<std::mutex> lock(mResultMutex);
		results.swap(mResults);
	}

	// Reap a finished run so its threads are joined and a new run can
	// start.  Done outside the result lock: the controller only sets
	// mFinished after every worker has been joined, so nothing can
	// still be pushing results, but joining under a lock a worker might
	// want is a deadlock waiting to be introduced by a later edit.
	if (mFinished.exchange(false, std::memory_order_acq_rel)) {
		if (mController.joinable())
			mController.join();
	}

	if (results.empty())
		return false;

	auto& entries = lib.GetEntries();
	bool changed = false;

	for (Result& r : results) {
		// Re-resolve by path.  The index is only a hint: a rescan that
		// completed while this result was in flight may have reordered
		// or removed entries, and applying blindly would attach one
		// game's metadata to another.
		int index = -1;
		if (r.mEntryIndex >= 0 && (size_t)r.mEntryIndex < entries.size()
			&& !entries[r.mEntryIndex].mVariants.empty()
			&& entries[r.mEntryIndex].mVariants[0].mPath == r.mVariantPath)
		{
			index = r.mEntryIndex;
		} else {
			index = lib.FindEntryByVariantPath(r.mVariantPath);
		}

		if (index < 0 || (size_t)index >= entries.size())
			continue;   // the game left the library mid-run

		GameEntry& e = entries[index];

		// Persist a CRC the worker had to compute, so it is never
		// recomputed — same lazy-fill contract the netplay path uses.
		if (r.mComputedCRC32 && !e.mVariants.empty()
			&& !e.mVariants[0].mGameFileCRC32)
		{
			e.mVariants[0].mGameFileCRC32 = r.mComputedCRC32;
			changed = true;
		}

		if (r.mbHaveMeta) {
			e.mMeta = std::move(r.mMeta);
			changed = true;
		}

		if (artCache) {
			for (const VDStringW& path : r.mInvalidatePaths)
				artCache->Invalidate(path);
		}
	}

	if (changed)
		lib.SaveCache();

	return changed;
}
