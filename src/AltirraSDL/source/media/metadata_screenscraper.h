//	AltirraSDL - ScreenScraper.fr metadata provider
//
//	Wire format and matching strategy for screenscraper.fr's api2.  The
//	network call and the response parse are separate entry points so the
//	parser can be exercised against saved fixtures with no network.
//
//	Matching, in order of confidence:
//
//	  1. CRC32 of the game file's raw bytes.  The Game Library already
//	     computes and persists this (GameVariant::mGameFileCRC32) for the
//	     netplay joiner, so we reuse it verbatim.  Exact and free.
//	  2. The file name, verbatim, via jeuInfos.php's `romnom`.  Note that
//	     ScreenScraper matches `romnom` EXACTLY — it is a database lookup,
//	     not a search — so this only helps when the file happens to be
//	     named the way the archive names it.
//	  3. The *cleaned* title (decorations stripped) through the same exact
//	     lookup, which catches "Ballblazer (1984)(Lucasfilm).atr" when the
//	     database has plain "Ballblazer".
//	  4. A real fuzzy search via jeuRecherche.php, whose candidates are
//	     then confirmed by similarity before being accepted and re-fetched
//	     by game id.
//
//	Steps 3 and 4 exist because an 8-bit library is mostly *not* pristine
//	archive dumps: it is cracked, trained, re-dumped and hand-renamed
//	files whose bytes and names both differ from anything indexed.  Step 4
//	costs an extra request per unmatched game, so it is behind a setting.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <vd2/system/VDString.h>

#include "../ui/gamelibrary/game_library.h"

struct ATMetadataSettings;

// ScreenScraper platform ids, confirmed against
// screenscraper.fr/systemeinfos.php.
enum : int {
	kATScreenScraperSystemAtari8Bit = 43,
	kATScreenScraperSystemAtari5200 = 40,
};

// How a request ended.  Distinguishes "ask again later" from "stop the
// whole run", because hammering a closed or quota-exhausted API is both
// useless and rude.
// Mapped from ScreenScraper's documented HTTP status codes; see
// ATScreenScraperClassifyError for the table.
enum class ATScreenScraperOutcome {
	Matched,
	NotFound,
	// Retryable for this one game.
	TransientError,
	// HTTP 429: the account's thread / per-minute allowance is used up.
	// NOT fatal — the documented remedy is "reduce the query speed", so
	// the engine backs off and retries rather than abandoning the run.
	RateLimited,
	// Abort the entire run and show a banner.
	QuotaExceeded,        // 430: daily scrape quota spent
	TooManyUnknownRoms,   // 431: too many unrecognised ROMs today
	ApiClosed,            // 401 / 423
	Blacklisted,          // 426: this software version is refused
	BadCredentials,       // 403
	NotConfigured,
	Unavailable,      // no HTTPS backend on this platform/build
};

// True when the outcome should stop the whole run rather than just skip
// the current game.
bool ATScreenScraperIsFatal(ATScreenScraperOutcome outcome);

// Human-readable, user-facing explanation for a fatal outcome.
const char *ATScreenScraperOutcomeText(ATScreenScraperOutcome outcome);

struct ATScreenScraperQuery {
	uint32_t  mCRC32 = 0;
	uint64_t  mFileSize = 0;
	VDStringA mRomName;          // UTF-8 file name including extension
	int       mSystemId = kATScreenScraperSystemAtari8Bit;

	// When non-zero the game is looked up directly by id, skipping all
	// matching.  Set after a manual "Search ScreenScraper" pick so a
	// later batch run cannot re-match the entry to something else.
	uint32_t  mPinnedGameId = 0;

	// Second pass with no CRC (filename + size only), used after a
	// CRC miss to catch re-dumps and cracked images.
	bool      mbNameOnly = false;
};

// Decode HTML entities in provider text, in place.
//
// ScreenScraper stores synopses and titles as HTML fragments: a
// description routinely contains &quot;, &amp;, &#39; and friends.  A web
// front end renders those; we draw the string literally, so without this
// the user sees the markup.  Handles the named entities that actually
// occur plus decimal and hex numeric references.
//
// Exposed (and pure) so the parser test can pin it.
void ATScreenScraperDecodeEntities(VDStringW& text);

// --- Name matching ----------------------------------------------------
//
// Pure string functions, no network, so they can be exercised directly
// by the parser test.

// Strip a game file name down to something a database might recognise:
// drop the extension, drop every "(...)" and "[...]" group (region,
// year, publisher, cracker and trainer tags), turn separators into
// spaces, and collapse runs of whitespace.
//
//   "Ballblazer (1984)(Lucasfilm Games)[cr TCS].atr" -> "Ballblazer"
//   "M.U.L.E._v2.xex"                               -> "M.U.L.E. v2"
VDStringA ATScreenScraperCleanName(const char *fileName);

// Fold a name to a comparison key: lower case, letters and digits only.
// "The Great Escape!" and "the-great_escape" both key to
// "thegreatescape", which is what makes a candidate comparison robust
// against punctuation and spacing differences.
VDStringA ATScreenScraperMatchKey(const char *name);

// 0..100 similarity between two match keys.  100 is an exact key match;
// otherwise this is a containment-and-length measure, deliberately
// conservative — attaching "Boulder Dash II" metadata to "Boulder Dash"
// is a worse outcome than finding nothing.
int ATScreenScraperNameScore(const char *keyA, const char *keyB);

// Minimum score for a fuzzy candidate to be accepted without a hash or
// an exact-name confirmation.
enum : int { kATScreenScraperMinFuzzyScore = 85 };

// --- Name search (jeuRecherche.php) -----------------------------------

struct ATScreenScraperCandidate {
	uint32_t  mGameId = 0;
	VDStringA mName;      // best regional name, for scoring and for logs
};

// Free-text search.  Returns Matched with `outCandidates` populated (a
// zero-length list is NotFound, not an error), or the usual fatal /
// retryable outcomes.
ATScreenScraperOutcome ATScreenScraperSearch(const VDStringA& name,
	int systemId, const ATMetadataSettings& settings,
	const std::atomic<bool> *cancel,
	std::vector<ATScreenScraperCandidate>& outCandidates,
	VDStringA& outError);

// Parse a jeuRecherche.php body.  Split out so the test can feed it a
// fixture.
bool ATScreenScraperParseSearchResults(const void *json, size_t length,
	std::vector<ATScreenScraperCandidate>& outCandidates,
	VDStringA& outError);

// Pick the best candidate for `wantedName`, or nullptr when none clears
// kATScreenScraperMinFuzzyScore.  `outScore` receives the winner's score.
const ATScreenScraperCandidate *ATScreenScraperPickBestCandidate(
	const std::vector<ATScreenScraperCandidate>& candidates,
	const char *wantedName, int& outScore);

// Provider output.  Text fields land straight in GameMetadata; media
// arrive as URLs, because downloading them is the scraper engine's job
// (it owns the file naming, the config dir and the cancel flag).
struct ATScreenScraperResult {
	GameMetadata mMeta;

	std::string mBoxArtUrl;
	std::string mTitleShotUrl;
	std::string mScreenshotUrl;
	std::string mLogoUrl;

	// Populated from the response's ssuser block when present, so the
	// engine can size its thread pool to what the account allows.
	int mMaxThreads = 0;
	int mRequestsToday = 0;
	int mMaxRequestsPerDay = 0;
};

// Account status, for the settings UI's "Test credentials" button.
struct ATScreenScraperAccount {
	bool      mbValid = false;
	VDStringA mLevel;
	int       mMaxThreads = 0;
	int       mRequestsToday = 0;
	int       mMaxRequestsPerDay = 0;
	VDStringA mError;
};

// Blocking.  Performs one jeuInfos.php request and parses the reply.
ATScreenScraperOutcome ATScreenScraperFetch(
	const ATScreenScraperQuery& query,
	const ATMetadataSettings& settings,
	const std::atomic<bool>* cancel,
	ATScreenScraperResult& out,
	VDStringA& outError);

// Blocking.  One ssuserInfos.php request.  Used by "Test credentials"
// and once at the start of a run to learn the thread allowance.
bool ATScreenScraperFetchAccount(
	const ATMetadataSettings& settings,
	const std::atomic<bool>* cancel,
	ATScreenScraperAccount& out);

// Pure parse of a jeuInfos.php JSON body.  No network, no globals —
// this is the entry point the fixture tests drive.
bool ATScreenScraperParseGameInfo(
	const void *json, size_t length,
	const char *regionPref, const char *languagePref,
	ATScreenScraperResult& out, VDStringA& outError);

// Classify a response body / HTTP status into an outcome.  Exposed for
// the fixture tests, which assert each error string maps correctly.
ATScreenScraperOutcome ATScreenScraperClassifyError(
	const void *body, size_t length, int httpStatus);
