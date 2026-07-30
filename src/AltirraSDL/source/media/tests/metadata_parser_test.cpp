//	AltirraSDL - ScreenScraper parser self-test
//
//	Drives ATScreenScraperParseGameInfo and ATScreenScraperClassifyError
//	against hand-written fixtures that mirror the shapes screenscraper.fr
//	actually returns: numbers as JSON strings, localized arrays keyed on
//	`region` or `langue`, media entries carrying their own region, and
//	the French error strings the API answers with.
//
//	No network, no registry, no ImGui — safe for CI.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// vdtypes must come first: text.h uses the vd2 fixed-width aliases.
#include <vd2/system/vdtypes.h>
#include <vd2/system/text.h>
#include <vd2/vdjson/jsonreader.h>
#include <vd2/vdjson/jsonvalue.h>
#include <vd2/vdjson/jsonwriter.h>
#include <vd2/vdjson/jsonoutput.h>

#include "metadata_screenscraper.h"
#include "metadata_settings.h"

// ---------------------------------------------------------------------------
// Stubs.  The parser under test never reaches these, but the translation
// unit references them, so the link needs definitions.  Keeping them
// here (rather than linking metadata_settings.cpp) also guarantees the
// test can never touch the real registry.
// ---------------------------------------------------------------------------

bool ATMetadataGetDevCredential(VDStringA& outDevId,
	VDStringA& outDevPassword)
{
	outDevId = VDStringA("testdev");
	outDevPassword = VDStringA("testpw");
	return true;
}

const char *ATMetadataGetDebugPassword() { return ""; }

// ---------------------------------------------------------------------------
// Tiny assertion harness
// ---------------------------------------------------------------------------

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char *what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("  FAIL: %s\n", what);
	}
}

void CheckStr(const VDStringW& actual, const char *expected, const char *what) {
	const VDStringA actualU8 = VDTextWToU8(actual);
	++g_checks;
	if (std::strcmp(actualU8.c_str(), expected) != 0) {
		++g_failures;
		std::printf("  FAIL: %s\n        expected \"%s\"\n        actual   \"%s\"\n",
			what, expected, actualU8.c_str());
	}
}

void CheckStr(const VDStringA& actual, const char *expected, const char *what) {
	++g_checks;
	if (std::strcmp(actual.c_str(), expected) != 0) {
		++g_failures;
		std::printf("  FAIL: %s\n        expected \"%s\"\n        actual   \"%s\"\n",
			what, expected, actual.c_str());
	}
}

void CheckStr(const std::string& actual, const char *expected, const char *what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("  FAIL: %s\n        expected \"%s\"\n        actual   \"%s\"\n",
			what, expected, actual.c_str());
	}
}

bool Parse(const char *json, const char *region, const char *language,
	ATScreenScraperResult& out)
{
	VDStringA error;
	const bool ok = ATScreenScraperParseGameInfo(json, std::strlen(json),
		region, language, out, error);
	if (!ok)
		std::printf("  (parse error: %s)\n",
			error.empty() ? "<none>" : error.c_str());
	return ok;
}

// A response shaped like a real Atari 8-bit hit.
const char *kFullHit = R"JSON({
  "header": { "APIversion": "2" },
  "response": {
    "ssuser": {
      "id": "tester", "niveau": "3", "maxthreads": "4",
      "requeststoday": "312", "maxrequestsperday": "20000"
    },
    "jeu": {
      "id": "12873",
      "noms": [
        { "region": "us",  "text": "Bruce Lee (USA)" },
        { "region": "wor", "text": "Bruce Lee" },
        { "region": "jp",  "text": "ブルース・リー" }
      ],
      "synopsis": [
        { "langue": "fr", "text": "Guidez Bruce Lee." },
        { "langue": "en", "text": "Guide Bruce Lee through 20 chambers." }
      ],
      "editeur":     { "id": "42", "text": "Datasoft" },
      "developpeur": { "id": "77", "text": "Ron J. Fortier" },
      "joueurs":     { "text": "1-2" },
      "note":        { "text": "16" },
      "dates": [
        { "region": "us",  "text": "1984-01-01" },
        { "region": "wor", "text": "1984" }
      ],
      "genres": [
        { "id": "1", "noms": [ { "langue": "en", "text": "Action" },
                               { "langue": "fr", "text": "Action" } ] },
        { "id": "2", "noms": [ { "langue": "en", "text": "Platform" } ] }
      ],
      "medias": [
        { "type": "box-2D", "region": "eu",  "url": "https://ss/eu-box.png" },
        { "type": "box-2D", "region": "wor", "url": "https://ss/wor-box.png" },
        { "type": "ss",      "region": "wor", "url": "https://ss/screen.png" },
        { "type": "sstitle", "region": "wor", "url": "https://ss/title.png" },
        { "type": "wheel",   "region": "wor", "url": "https://ss/wheel.png" }
      ]
    }
  }
})JSON";

void TestFullHit() {
	std::printf("full Atari 8-bit hit\n");
	ATScreenScraperResult r;
	Check(Parse(kFullHit, "wor", "en", r), "parses");

	Check(r.mMeta.mStatus == GameMetaStatus::Matched, "status is Matched");
	Check(r.mMeta.mProviderGameId == 12873, "game id parsed from string");
	CheckStr(r.mMeta.mTitle, "Bruce Lee", "world title preferred");
	CheckStr(r.mMeta.mDescription,
		"Guide Bruce Lee through 20 chambers.", "English synopsis preferred");
	CheckStr(r.mMeta.mPublisher, "Datasoft", "publisher from {text}");
	CheckStr(r.mMeta.mDeveloper, "Ron J. Fortier", "developer from {text}");
	Check(r.mMeta.mYear == 1984, "year from world date");
	Check(r.mMeta.mPlayersMax == 2, "max players from \"1-2\"");
	Check(r.mMeta.mRating == 16, "rating from string");
	CheckStr(r.mMeta.mGenre, "Action, Platform", "genres joined");

	CheckStr(r.mBoxArtUrl, "https://ss/wor-box.png", "box art region wor");
	CheckStr(r.mScreenshotUrl, "https://ss/screen.png", "screenshot");
	CheckStr(r.mTitleShotUrl, "https://ss/title.png", "title screen");
	CheckStr(r.mLogoUrl, "https://ss/wheel.png", "wheel");

	Check(r.mMaxThreads == 4, "ssuser maxthreads");
	Check(r.mRequestsToday == 312, "ssuser requeststoday");
	Check(r.mMaxRequestsPerDay == 20000, "ssuser maxrequestsperday");
}

void TestRegionPreference() {
	std::printf("region / language preference\n");
	ATScreenScraperResult r;
	Check(Parse(kFullHit, "us", "fr", r), "parses");
	CheckStr(r.mMeta.mTitle, "Bruce Lee (USA)", "US title when US preferred");
	CheckStr(r.mMeta.mDescription, "Guidez Bruce Lee.",
		"French synopsis when French preferred");
	// "Platform" has no French name in the fixture.  Falling back to
	// whatever text exists is deliberate: a genre in the wrong language
	// is more useful than a blank one, and the same fallback keeps
	// titles from disappearing for regions with sparse coverage.
	CheckStr(r.mMeta.mGenre, "Action, Platform",
		"per-entry language fallback when a genre lacks the preferred name");
	Check(r.mMeta.mYear == 1984, "US date still yields the year");
	// No US box exists; selection must fall through to another region
	// rather than dropping the slot.
	CheckStr(r.mBoxArtUrl, "https://ss/wor-box.png",
		"box falls back to world when preferred region is absent");
}

void TestNoMedia() {
	std::printf("hit with no media\n");
	const char *json = R"JSON({"response":{"jeu":{
		"id":"5","noms":[{"region":"wor","text":"Nameless"}]}}})JSON";
	ATScreenScraperResult r;
	Check(Parse(json, "wor", "en", r), "parses");
	CheckStr(r.mMeta.mTitle, "Nameless", "title present");
	Check(r.mBoxArtUrl.empty() && r.mScreenshotUrl.empty(), "no media urls");
	Check(!r.mMeta.HasAnyMedia(), "HasAnyMedia false");
	Check(r.mMeta.HasAnyText(), "HasAnyText true");
}

void TestBox3DFallback() {
	std::printf("only box-3D available\n");
	const char *json = R"JSON({"response":{"jeu":{"id":"7",
		"noms":[{"region":"wor","text":"Boxed"}],
		"medias":[{"type":"box-3D","region":"wor","url":"https://ss/3d.png"}]}}})JSON";
	ATScreenScraperResult r;
	Check(Parse(json, "wor", "en", r), "parses");
	CheckStr(r.mBoxArtUrl, "https://ss/3d.png", "box-3D used for the box slot");
}

void TestMediaWithoutRegion() {
	std::printf("media entries with no region field\n");
	const char *json = R"JSON({"response":{"jeu":{"id":"8",
		"noms":[{"region":"wor","text":"Regionless"}],
		"medias":[{"type":"box-2D","url":"https://ss/plain.png"}]}}})JSON";
	ATScreenScraperResult r;
	Check(Parse(json, "wor", "en", r), "parses");
	CheckStr(r.mBoxArtUrl, "https://ss/plain.png",
		"region-less media still selected");
}

void TestTitleScreenDeduplication() {
	std::printf("title screen and screenshot resolving to the same image\n");
	// "ss" is the fallback for both slots.  Showing the same picture
	// twice in the details view reads as a rendering bug, so the title
	// slot must drop out.
	const char *json = R"JSON({"response":{"jeu":{"id":"9",
		"noms":[{"region":"wor","text":"Dup"}],
		"medias":[{"type":"ss","region":"wor","url":"https://ss/only.png"}]}}})JSON";
	ATScreenScraperResult r;
	Check(Parse(json, "wor", "en", r), "parses");
	CheckStr(r.mScreenshotUrl, "https://ss/only.png", "screenshot keeps it");
	Check(r.mTitleShotUrl.empty(), "title slot cleared");
}

void TestUtf8Accents() {
	std::printf("UTF-8 accented text survives the round trip\n");
	const char *json =
		"{\"response\":{\"jeu\":{\"id\":\"10\","
		"\"noms\":[{\"region\":\"wor\",\"text\":\"Le Fran\xC3\xA7""ais\"}],"
		"\"editeur\":{\"text\":\"\xC3\x89""diteur\"}}}}";
	ATScreenScraperResult r;
	Check(Parse(json, "wor", "en", r), "parses");
	CheckStr(r.mMeta.mTitle, "Le Fran\xC3\xA7""ais", "accented title");
	CheckStr(r.mMeta.mPublisher, "\xC3\x89""diteur", "accented publisher");
}

void TestSlashDate() {
	std::printf("date in MM/YYYY form\n");
	const char *json = R"JSON({"response":{"jeu":{"id":"11",
		"noms":[{"region":"wor","text":"Dated"}],
		"dates":[{"region":"wor","text":"03/1983"}]}}})JSON";
	ATScreenScraperResult r;
	Check(Parse(json, "wor", "en", r), "parses");
	Check(r.mMeta.mYear == 1983, "year extracted from a non-leading position");
}

void TestNoGame() {
	std::printf("response with no game\n");
	const char *json = R"JSON({"response":{"ssuser":{"maxthreads":"1"}}})JSON";
	ATScreenScraperResult r;
	VDStringA error;
	const bool ok = ATScreenScraperParseGameInfo(json, std::strlen(json),
		"wor", "en", r, error);
	Check(!ok, "reports failure");
	CheckStr(error, "no game in response",
		"error text is the one the caller maps to NotFound");
}

void TestTruncatedJson() {
	std::printf("truncated JSON\n");
	const char *json = "{\"response\":{\"jeu\":{\"id\":\"12\",";
	ATScreenScraperResult r;
	VDStringA error;
	Check(!ATScreenScraperParseGameInfo(json, std::strlen(json),
		"wor", "en", r, error), "reports failure");
	Check(!error.empty(), "sets an error message");
}

void TestEmptyBody() {
	std::printf("empty body\n");
	ATScreenScraperResult r;
	VDStringA error;
	Check(!ATScreenScraperParseGameInfo("", 0, "wor", "en", r, error),
		"reports failure");
}

void TestErrorClassification() {
	std::printf("error classification\n");
	struct Case {
		const char *body;
		int status;
		ATScreenScraperOutcome expected;
		const char *what;
	};

	static const Case cases[] = {
		{ "Erreur : Rom/Jeu non trouvee !", 200,
		  ATScreenScraperOutcome::NotFound, "rom not found" },
		{ "Erreur : Rom/Jeu non trouv\xC3\xA9""e !", 200,
		  ATScreenScraperOutcome::NotFound, "rom not found (accented)" },
		{ "Votre quota de scrape est depasse", 200,
		  ATScreenScraperOutcome::QuotaExceeded, "quota exhausted" },
		{ "API totalement ferm\xC3\xA9""e", 200,
		  ATScreenScraperOutcome::ApiClosed, "api closed" },
		{ "Le logiciel de scrape utilise a ete blackliste", 200,
		  ATScreenScraperOutcome::Blacklisted, "blacklisted" },
		{ "Erreur de login : Verifiez vos identifiants", 200,
		  ATScreenScraperOutcome::BadCredentials, "bad login" },
		// ScreenScraper's published API v2 status table.  These are NOT
		// the conventional HTTP meanings, so each one is pinned.
		{ "", 200, ATScreenScraperOutcome::Matched,            "200 ok" },
		{ "", 400, ATScreenScraperOutcome::TransientError,     "400 bad request" },
		{ "", 401, ATScreenScraperOutcome::ApiClosed,          "401 api closed to non-members" },
		{ "", 403, ATScreenScraperOutcome::BadCredentials,     "403 dev login error" },
		{ "", 404, ATScreenScraperOutcome::NotFound,           "404 game not found" },
		{ "", 423, ATScreenScraperOutcome::ApiClosed,          "423 api fully closed" },
		{ "", 426, ATScreenScraperOutcome::Blacklisted,        "426 software blacklisted" },
		{ "", 429, ATScreenScraperOutcome::RateLimited,        "429 thread limit" },
		{ "", 430, ATScreenScraperOutcome::QuotaExceeded,      "430 daily quota" },
		{ "", 431, ATScreenScraperOutcome::TooManyUnknownRoms, "431 too many unknown roms" },
		{ "", 503, ATScreenScraperOutcome::TransientError,     "503 unmapped" },
	};

	for (const Case& c : cases) {
		const ATScreenScraperOutcome got = ATScreenScraperClassifyError(
			c.body, std::strlen(c.body), c.status);
		++g_checks;
		if (got != c.expected) {
			++g_failures;
			std::printf("  FAIL: %s -> %d (expected %d)\n",
				c.what, (int)got, (int)c.expected);
		}
	}

	// Everything that stops a run must be flagged fatal, and everything
	// that does not must not be — the engine branches on exactly this.
	Check(ATScreenScraperIsFatal(ATScreenScraperOutcome::QuotaExceeded),
		"quota is fatal");
	Check(ATScreenScraperIsFatal(ATScreenScraperOutcome::ApiClosed),
		"api closed is fatal");
	Check(ATScreenScraperIsFatal(ATScreenScraperOutcome::Blacklisted),
		"blacklist is fatal");
	Check(ATScreenScraperIsFatal(ATScreenScraperOutcome::TooManyUnknownRoms),
		"too-many-unknown-roms is fatal");
	Check(!ATScreenScraperIsFatal(ATScreenScraperOutcome::NotFound),
		"not-found is NOT fatal");
	Check(!ATScreenScraperIsFatal(ATScreenScraperOutcome::TransientError),
		"transient is NOT fatal");
	// 429 is a "slow down" signal, not a "stop".  Treating it as fatal
	// would abandon a whole library run over a momentary thread cap.
	Check(!ATScreenScraperIsFatal(ATScreenScraperOutcome::RateLimited),
		"rate-limited is NOT fatal (the remedy is to back off)");

	// Every fatal outcome owes the user an explanation; a blank banner
	// would leave the UI showing an empty warning strip.
	const ATScreenScraperOutcome fatals[] = {
		ATScreenScraperOutcome::QuotaExceeded,
		ATScreenScraperOutcome::TooManyUnknownRoms,
		ATScreenScraperOutcome::ApiClosed,
		ATScreenScraperOutcome::Blacklisted,
		ATScreenScraperOutcome::BadCredentials,
		ATScreenScraperOutcome::NotConfigured,
		ATScreenScraperOutcome::Unavailable,
	};
	for (ATScreenScraperOutcome o : fatals) {
		const char *text = ATScreenScraperOutcomeText(o);
		Check(text && *text, "fatal outcome has user-facing text");
	}
}

// The Game Library cache is written by VDJSONWriter and read back by
// VDJSONReader.  The writer emits non-ASCII as raw UTF-8 (it does not
// \u-escape), so any accented game name puts multi-byte sequences into
// the file — and a reader that cannot decode them silently fails the
// whole document, losing the user's play history, custom art and
// metadata on every restart.  This is a regression guard for exactly
// that: it is the shape game_library.cpp uses, not a synthetic case.
void TestCacheRoundTrip() {
	std::printf("JSON round trip with non-ASCII (Game Library cache shape)\n");

	// U+00E7 (2-byte UTF-8), U+30D6 (3-byte), U+007F boundary, and a
	// character just below the surrogate range.
	static const wchar_t kName[] = {
		L'F', L'r', L'a', L'n', 0x00E7, L'a', L'i', L's', L' ',
		0x00C9, L'd', L'i', L't', 0x2014, 0x30D6, 0xD7FF, 0
	};

	VDStringW json;
	{
		VDJSONStringOutput out(json);
		VDJSONWriter writer;
		writer.Begin(&out, false);
		writer.OpenObject();
		writer.WriteMemberName(L"displayName");
		writer.WriteString(kName);
		writer.Close();
		writer.End();
	}

	// The writer produces wide chars; the cache goes to disk as UTF-8,
	// so encode before parsing, exactly like reading the file back.
	const VDStringA utf8 = VDTextWToU8(json);

	VDJSONDocument doc;
	VDJSONReader reader;
	Check(reader.Parse(utf8.data(), utf8.size(), doc),
		"non-ASCII document parses");

	auto root = doc.Root();
	Check(root.IsObject(), "root is an object");
	auto name = root[L"displayName"];
	Check(name.IsString(), "displayName is a string");
	if (name.IsString()) {
		Check(wcscmp(name.AsString(), kName) == 0,
			"round-tripped string is byte-identical");
	}
}

// The error classifier scans the response body for the provider's error
// strings.  A SUCCESSFUL reply embeds the game's synopsis, so a body can
// legitimately contain "quota" or "non trouvee" as prose.  If the
// classifier ran first, such a game would abort the whole run (quota) or
// be recorded as a miss.  ATScreenScraperFetch therefore parses first
// and only classifies when there is no usable game record.  This test
// pins both halves of that contract.
void TestErrorWordsInsideSynopsis() {
	std::printf("error words appearing inside a successful synopsis\n");

	const char *json = R"JSON({"response":{"jeu":{
		"id":"99",
		"noms":[{"region":"wor","text":"Quota Quest"}],
		"synopsis":[{"langue":"en","text":
			"Your quota of lanterns is never trouve; the manual says non trouvee."}],
		"medias":[{"type":"box-2D","region":"wor","url":"https://ss/q.png"}]
	}}})JSON";

	// The parser must succeed: it is what makes this a match.
	ATScreenScraperResult r;
	Check(Parse(json, "wor", "en", r), "parses despite the error words");
	CheckStr(r.mMeta.mTitle, "Quota Quest", "title survives");
	Check(r.mMeta.mStatus == GameMetaStatus::Matched, "status is Matched");

	// And the classifier, run on the same bytes, really would have
	// mislabelled it — which is precisely why the ordering matters.
	const ATScreenScraperOutcome misread = ATScreenScraperClassifyError(
		json, std::strlen(json), 200);
	Check(misread != ATScreenScraperOutcome::Matched,
		"classifier alone would misread this body (ordering is load-bearing)");
}


// ---------------------------------------------------------------------------
// Name matching
//
// This is the layer that decides whether a hand-renamed, cracked or
// re-dumped file finds its game.  It is also the layer most able to
// attach the WRONG game's cover to a ROM, so the negative cases matter
// at least as much as the positive ones.
// ---------------------------------------------------------------------------

void TestCleanName() {
	CheckStr(ATScreenScraperCleanName(
		"Ballblazer (1984)(Lucasfilm Games)[cr TCS].atr"),
		"Ballblazer", "clean: strips year, publisher, cracker tag, ext");

	CheckStr(ATScreenScraperCleanName("Boulder Dash [!].xex"),
		"Boulder Dash", "clean: strips quality mark");

	CheckStr(ATScreenScraperCleanName("Star_Raiders-PAL.car"),
		"Star Raiders PAL", "clean: separators become spaces");

	// A dot is load-bearing in Atari titles; folding it would merge
	// distinct games.
	CheckStr(ATScreenScraperCleanName("M.U.L.E..atr"),
		"M.U.L.E.", "clean: keeps internal dots, drops only the extension");

	// A long trailing component is part of the title, not an extension.
	CheckStr(ATScreenScraperCleanName("Conan the Barbarian"),
		"Conan the Barbarian", "clean: no extension to strip");

	CheckStr(ATScreenScraperCleanName("Rescue on Fractalus! (Rev A (PAL)).atr"),
		"Rescue on Fractalus!", "clean: nested brackets");

	CheckStr(ATScreenScraperCleanName("   Spaced   Out   .xex"),
		"Spaced Out", "clean: collapses and trims whitespace");

	CheckStr(ATScreenScraperCleanName(""), "", "clean: empty input");
	CheckStr(ATScreenScraperCleanName(nullptr), "", "clean: null input");
}

void TestMatchKey() {
	CheckStr(ATScreenScraperMatchKey("The Great Escape!"), "thegreatescape",
		"key: case, spaces and punctuation folded away");
	CheckStr(ATScreenScraperMatchKey("M.U.L.E."), "mule", "key: dots folded");
	CheckStr(ATScreenScraperMatchKey("Zaxxon 2"), "zaxxon2",
		"key: digits kept — they distinguish sequels");
}

void TestNameScore() {
	Check(ATScreenScraperNameScore("ballblazer", "ballblazer") == 100,
		"score: identical keys score 100");

	// The guard that matters: a sequel must NOT clear the acceptance bar.
	const int sequel = ATScreenScraperNameScore("boulderdash", "boulderdashii");
	Check(sequel > 0 && sequel < kATScreenScraperMinFuzzyScore,
		"score: 'Boulder Dash' vs 'Boulder Dash II' stays below threshold");

	Check(ATScreenScraperNameScore("zaxxon", "bluemax")
		< kATScreenScraperMinFuzzyScore,
		"score: unrelated titles score low");

	Check(ATScreenScraperNameScore("", "ballblazer") == 0,
		"score: empty key scores 0");
}

void TestSearchParse() {
	static const char kJson[] =
		"{\"response\":{\"jeux\":["
		"{\"id\":\"1234\",\"noms\":["
		"{\"region\":\"us\",\"text\":\"Ballblazer\"},"
		"{\"region\":\"jp\",\"text\":\"Ballblazer JP\"}]},"
		"{\"id\":\"5678\",\"nom\":\"Ballblazer II\"}"
		"]}}";

	std::vector<ATScreenScraperCandidate> cands;
	VDStringA err;
	Check(ATScreenScraperParseSearchResults(kJson, sizeof(kJson) - 1,
		cands, err), "search: parses a well-formed reply");
	// Two regional names for game 1234 plus the bare-nom form for 5678.
	Check(cands.size() == 3, "search: every regional name is a candidate");

	int score = 0;
	const ATScreenScraperCandidate *best =
		ATScreenScraperPickBestCandidate(cands, "Ballblazer", score);
	Check(best != nullptr && best->mGameId == 1234 && score == 100,
		"search: exact title wins over the sequel");

	// A file whose cleaned name matches nothing in the list must be
	// rejected rather than assigned the closest thing available.
	best = ATScreenScraperPickBestCandidate(cands, "Pitfall II", score);
	Check(best == nullptr, "search: no candidate clears the bar -> reject");
}

void TestSearchParseEmpty() {
	static const char kNoResults[] = "{\"response\":{}}";
	std::vector<ATScreenScraperCandidate> cands;
	VDStringA err;
	Check(ATScreenScraperParseSearchResults(kNoResults,
		sizeof(kNoResults) - 1, cands, err),
		"search: a no-results reply is not an error");
	Check(cands.empty(), "search: no-results reply yields no candidates");

	Check(!ATScreenScraperParseSearchResults("{oops", 5, cands, err),
		"search: malformed JSON is rejected");
}


// Regression: vdjson's object parser tested the wrong closing character,
// so `{}` — and any document containing one at any depth — failed to
// parse.  ScreenScraper replies do contain empty objects, and every
// consumer of the Game Library cache shares this reader, so this guards
// an upstream resync silently reintroducing it.
void TestEmptyJsonObject() {
	std::printf("empty JSON objects parse (vdjson regression)\n");

	struct Case { const char *json; const char *what; };
	static const Case kCases[] = {
		{ "{}",                        "bare empty object" },
		{ "{\"a\":{}}",                "empty object as a member" },
		{ "{\"a\":{},\"b\":1}",        "empty object followed by a member" },
		{ "{\"a\":[{}]}",              "empty object inside an array" },
		{ "[]",                        "bare empty array (was already ok)" },
	};

	for (const Case& c : kCases) {
		VDJSONDocument doc;
		VDJSONReader reader;
		Check(reader.Parse(c.json, std::strlen(c.json), doc), c.what);
	}

	// And the value has to be usable, not merely accepted.
	const char *nested = "{\"response\":{}}";
	VDJSONDocument doc;
	VDJSONReader reader;
	if (reader.Parse(nested, std::strlen(nested), doc)) {
		auto resp = doc.Root()[L"response"];
		Check(resp.IsObject(), "empty object round-trips as an object");
		Check(!resp[L"missing"].IsValid(),
			"absent member of an empty object reads as invalid");
	} else {
		Check(false, "nested empty object parses");
	}
}


// ---------------------------------------------------------------------------
// HTML entities
//
// ScreenScraper stores synopses as HTML fragments, so a description full
// of &quot; is what the user actually sees without this.
// ---------------------------------------------------------------------------

void TestDecodeEntities() {
	std::printf("HTML entity decoding\n");

	struct Case { const wchar_t *in; const wchar_t *out; const char *what; };
	static const Case kCases[] = {
		{ L"He said &quot;hello&quot;", L"He said \"hello\"",
		  "named: &quot;" },
		{ L"Ben &amp; Jerry", L"Ben & Jerry", "named: &amp;" },
		{ L"a &lt; b &gt; c", L"a < b > c", "named: &lt; &gt;" },
		{ L"It&#39;s here", L"It's here", "numeric decimal" },
		{ L"It&#x27;s here", L"It's here", "numeric hex" },
		{ L"nothing to do", L"nothing to do", "no entities: untouched" },
		{ L"", L"", "empty" },
		// An unterminated or unknown entity must survive verbatim.
		{ L"100% & rising", L"100% & rising", "bare ampersand survives" },
		{ L"&notanentity; x", L"&notanentity; x", "unknown name survives" },
		{ L"AT&T", L"AT&T", "ampersand with no semicolon" },
		// &amp; is decoded last, so an escaped entity stays escaped
		// rather than decoding twice into a bare quote.
		{ L"&amp;quot;", L"&quot;", "double-escaped entity decodes once" },
	};

	for (const Case& c : kCases) {
		VDStringW text(c.in);
		ATScreenScraperDecodeEntities(text);
		CheckStr(text, VDTextWToU8(VDStringW(c.out)).c_str(), c.what);
	}
}

void TestDecodeEntitiesInParse() {
	std::printf("provider text is decoded on the way in\n");
	const char *json = R"JSON({"response":{"jeu":{
		"id":"7","noms":[{"region":"wor","text":"Pitfall! &amp; Friends"}],
		"synopsis":[{"langue":"en","text":"They said &quot;run&quot; &#8212; so he did."}],
		"editeur":{"text":"Activision &amp; Co"}
	}}})JSON";

	ATScreenScraperResult r;
	VDStringA error;
	Check(ATScreenScraperParseGameInfo(json, std::strlen(json), "wor", "en",
		r, error), "parses");
	CheckStr(r.mMeta.mTitle, "Pitfall! & Friends", "title decoded");
	CheckStr(r.mMeta.mPublisher, "Activision & Co", "publisher decoded");
	Check(r.mMeta.mDescription.find(L"\"run\"") != VDStringW::npos,
		"synopsis quotes decoded");
	Check(r.mMeta.mDescription.find(L"&quot;") == VDStringW::npos,
		"no raw entity left in the synopsis");
}

}  // namespace

int main() {
	TestFullHit();
	TestRegionPreference();
	TestNoMedia();
	TestBox3DFallback();
	TestMediaWithoutRegion();
	TestTitleScreenDeduplication();
	TestUtf8Accents();
	TestSlashDate();
	TestNoGame();
	TestTruncatedJson();
	TestEmptyBody();
	TestErrorClassification();
	TestErrorWordsInsideSynopsis();
	TestCacheRoundTrip();
	TestEmptyJsonObject();
	TestDecodeEntities();
	TestDecodeEntitiesInParse();
	TestCleanName();
	TestMatchKey();
	TestNameScore();
	TestSearchParse();
	TestSearchParseEmpty();

	std::printf("\n%d checks, %d failure%s\n", g_checks, g_failures,
		g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
