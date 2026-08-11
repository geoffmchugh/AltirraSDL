//	AltirraSDL - ScreenScraper.fr metadata provider
//	See metadata_screenscraper.h for the contract.

#include <stdafx.h>

#include "metadata_screenscraper.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <vd2/system/text.h>
#include <vd2/vdjson/jsonreader.h>
#include <vd2/vdjson/jsonvalue.h>

#include "http_client.h"
#include "metadata_settings.h"

namespace {

// The host ScreenScraper documents for API v2.
const char kApiBase[] = "https://api.screenscraper.fr/api2/";

// Sent as softname and User-Agent.  ScreenScraper uses this to attribute
// traffic to a frontend, which is the whole point of the developer
// credential.
const char kSoftName[] = "AltirraSDL";

// -------------------------------------------------------------------------
// Small string helpers
// -------------------------------------------------------------------------

bool ContainsCI(const char *haystack, size_t n, const char *needle) {
	const size_t m = strlen(needle);
	if (!m || n < m)
		return false;
	for (size_t i = 0; i + m <= n; ++i) {
		size_t j = 0;
		while (j < m) {
			char a = haystack[i + j];
			char b = needle[j];
			if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
			if (a != b)
				break;
			++j;
		}
		if (j == m)
			return true;
	}
	return false;
}

void AppendParam(std::string& url, const char *name, const char *value) {
	if (!value || !*value)
		return;
	url += '&';
	url += name;
	url += '=';
	ATHttp::UrlEncode(value, strlen(value), url);
}

void AppendParamU64(std::string& url, const char *name, uint64_t value) {
	char buf[32];
	snprintf(buf, sizeof buf, "%llu", (unsigned long long)value);
	url += '&';
	url += name;
	url += '=';
	url += buf;
}

// ScreenScraper reports numbers as JSON strings ("16", "1-2", "1984-01-01").
int ParseLeadingInt(const wchar_t *s) {
	if (!s)
		return 0;
	while (*s == L' ')
		++s;
	int v = 0;
	bool any = false;
	while (*s >= L'0' && *s <= L'9') {
		v = v * 10 + (int)(*s - L'0');
		++s;
		any = true;
		if (v > 1000000)
			break;
	}
	return any ? v : 0;
}

// "1-2" -> 2, "4" -> 4, "1 or 2" -> 2.  We only surface a maximum.
int ParseMaxPlayers(const wchar_t *s) {
	if (!s)
		return 0;
	int best = 0;
	while (*s) {
		if (*s >= L'0' && *s <= L'9') {
			int v = 0;
			while (*s >= L'0' && *s <= L'9') {
				v = v * 10 + (int)(*s - L'0');
				++s;
				if (v > 99) { v = 99; break; }
			}
			if (v > best)
				best = v;
		} else {
			++s;
		}
	}
	return best;
}

// -------------------------------------------------------------------------
// Region / language preference
//
// ScreenScraper returns arrays of {region|langue, text} objects.  We walk
// a preference chain rather than taking element 0, otherwise an Atari
// title can come back in whichever language happens to sort first.
// -------------------------------------------------------------------------

const wchar_t *PickLocalized(const VDJSONValueRef& array,
	const char *keyName, const char *preferred)
{
	if (!array.IsArray())
		return nullptr;

	// Preference chain: the user's choice, then world, then the big
	// three regions.  Anything at all beats returning nothing.
	const char *chain[6] = { preferred, "wor", "us", "eu", "jp", nullptr };

	const VDStringW keyW = VDTextAToW(keyName);

	for (int level = 0; level < 5; ++level) {
		const char *want = chain[level];
		if (!want || !*want)
			continue;
		const VDStringW wantW = VDTextAToW(want);

		for (const auto& item : array.AsArray()) {
			auto key = item[keyW.c_str()];
			auto text = item[L"text"];
			if (!text.IsString())
				continue;
			if (key.IsString() && wcscmp(key.AsString(), wantW.c_str()) == 0)
				return text.AsString();
		}
	}

	// Last resort: the first entry with any text at all.
	for (const auto& item : array.AsArray()) {
		auto text = item[L"text"];
		if (text.IsString() && text.AsString()[0])
			return text.AsString();
	}
	return nullptr;
}

// Some fields are a bare object rather than an array: {"text":"Datasoft"}.
const wchar_t *PickText(const VDJSONValueRef& value) {
	if (value.IsString())
		return value.AsString();
	if (value.IsObject()) {
		auto text = value[L"text"];
		if (text.IsString())
			return text.AsString();
	}
	return nullptr;
}

// -------------------------------------------------------------------------
// Media selection
// -------------------------------------------------------------------------

struct MediaPick {
	// Candidate ScreenScraper media type names, most-wanted first.
	const char *const *mTypes;
	int mTypeCount;
	std::string *mOut;
};

void SelectMedia(const VDJSONValueRef& medias, const char *regionPref,
	const MediaPick *picks, int pickCount)
{
	if (!medias.IsArray())
		return;

	const char *regionChain[5] = { regionPref, "wor", "us", "eu", "jp" };

	for (int p = 0; p < pickCount; ++p) {
		const MediaPick& pick = picks[p];
		bool done = false;

		// Type is the strong preference (a box scan is never an
		// acceptable substitute for a title screen), region only breaks
		// ties within a type.
		for (int t = 0; t < pick.mTypeCount && !done; ++t) {
			const VDStringW wantType = VDTextAToW(pick.mTypes[t]);

			for (int r = 0; r < 6 && !done; ++r) {
				// r == 5 means "any region", for media with no region.
				const VDStringW wantRegion = (r < 5 && regionChain[r])
					? VDTextAToW(regionChain[r]) : VDStringW();

				for (const auto& m : medias.AsArray()) {
					auto type = m[L"type"];
					auto url = m[L"url"];
					if (!type.IsString() || !url.IsString())
						continue;
					if (wcscmp(type.AsString(), wantType.c_str()) != 0)
						continue;

					if (r < 5) {
						auto region = m[L"region"];
						if (!region.IsString())
							continue;
						if (wcscmp(region.AsString(), wantRegion.c_str()) != 0)
							continue;
					}

					const wchar_t *u = url.AsString();
					if (!u || !*u)
						continue;

					*pick.mOut = VDTextWToU8(VDStringW(u)).c_str();
					done = true;
					break;
				}
			}
		}
	}
}

// -------------------------------------------------------------------------
// URL assembly
// -------------------------------------------------------------------------

// Builds the credential prefix shared by every endpoint.  Returns false
// when the build has no developer credential and the user supplied none.
bool BuildAuthQuery(const ATMetadataSettings& settings, std::string& url) {
	VDStringA devId, devPassword;
	if (!ATMetadataGetDevCredential(devId, devPassword))
		return false;

	url += "devid=";
	ATHttp::UrlEncode(devId.c_str(), devId.size(), url);
	url += "&devpassword=";
	ATHttp::UrlEncode(devPassword.c_str(), devPassword.size(), url);
	AppendParam(url, "softname", kSoftName);
	AppendParam(url, "output", "json");

	// User account is optional.  When it is absent the parameters are
	// omitted entirely rather than sent empty — an empty sspassword is
	// treated as a failed login, not as an anonymous request.
	if (settings.mbUseUserAccount
		&& !settings.mUserName.empty()
		&& !settings.mUserPassword.empty())
	{
		AppendParam(url, "ssid", settings.mUserName.c_str());
		AppendParam(url, "sspassword", settings.mUserPassword.c_str());
	}

	// Developer debug mode.  Empty in every shipping build; see
	// metadata_settings.h.  Used only to exercise the quota and
	// thread-limit paths against the live server.
	const char *debugPassword = ATMetadataGetDebugPassword();
	if (debugPassword && *debugPassword)
		AppendParam(url, "devdebugpassword", debugPassword);

	return true;
}

std::string UserAgent() {
	std::string ua = kSoftName;
	ua += "/1.0";
	return ua;
}

}  // namespace

// ---------------------------------------------------------------------------
// Outcome helpers
// ---------------------------------------------------------------------------

bool ATScreenScraperIsFatal(ATScreenScraperOutcome outcome) {
	switch (outcome) {
		case ATScreenScraperOutcome::QuotaExceeded:
		case ATScreenScraperOutcome::TooManyUnknownRoms:
		case ATScreenScraperOutcome::ApiClosed:
		case ATScreenScraperOutcome::Blacklisted:
		case ATScreenScraperOutcome::BadCredentials:
		case ATScreenScraperOutcome::NotConfigured:
		case ATScreenScraperOutcome::Unavailable:
			return true;
		default:
			// RateLimited is deliberately NOT fatal: the documented
			// remedy is to slow down, not to stop.
			return false;
	}
}

const char *ATScreenScraperOutcomeText(ATScreenScraperOutcome outcome) {
	switch (outcome) {
		case ATScreenScraperOutcome::QuotaExceeded:
			return "ScreenScraper: the daily download quota is used up. "
			       "Anonymous downloads share one allowance across all "
			       "AltirraSDL users \xE2\x80\x94 a free ScreenScraper "
			       "account gives you your own.";
		case ATScreenScraperOutcome::TooManyUnknownRoms:
			return "ScreenScraper: too many files today that it does not "
			       "recognise. It asks that you check those files and try "
			       "again tomorrow.";
		case ATScreenScraperOutcome::RateLimited:
			return "ScreenScraper is asking for a slower download rate.";
		case ATScreenScraperOutcome::ApiClosed:
			return "ScreenScraper is closed or overloaded right now. "
			       "Please try again later.";
		case ATScreenScraperOutcome::Blacklisted:
			return "ScreenScraper has blocked this version of AltirraSDL. "
			       "Please report this so it can be fixed.";
		case ATScreenScraperOutcome::BadCredentials:
			return "ScreenScraper rejected the account details. Check the "
			       "username and password on the Metadata page.";
		case ATScreenScraperOutcome::NotConfigured:
			return "Metadata download is not configured in this build. "
			       "You can add your own ScreenScraper developer "
			       "credential under Advanced.";
		case ATScreenScraperOutcome::Unavailable:
			return "Metadata download is not available in this build "
			       "(no HTTPS support).";
		default:
			return "";
	}
}

// ---------------------------------------------------------------------------
// Error classification
// ---------------------------------------------------------------------------

ATScreenScraperOutcome ATScreenScraperClassifyError(
	const void *body, size_t length, int httpStatus)
{
	const char *text = (const char *)body;
	// The interesting markers are always in the first part of the
	// reply; scanning a whole media blob would be wasteful.
	const size_t n = length > 4096 ? 4096 : length;

	if (text && n) {
		// Order matters: "closed" and "blacklisted" are checked before
		// quota because a closed API also mentions quota wording.
		if (ContainsCI(text, n, "API totalement ferm"))
			return ATScreenScraperOutcome::ApiClosed;
		if (ContainsCI(text, n, "API fermee")
			|| ContainsCI(text, n, "API ferm\xC3\xA9""e"))
			return ATScreenScraperOutcome::ApiClosed;
		if (ContainsCI(text, n, "blacklist"))
			return ATScreenScraperOutcome::Blacklisted;
		if (ContainsCI(text, n, "quota"))
			return ATScreenScraperOutcome::QuotaExceeded;
		if (ContainsCI(text, n, "Erreur de login")
			|| ContainsCI(text, n, "login ou mot de passe"))
			return ATScreenScraperOutcome::BadCredentials;
		if (ContainsCI(text, n, "non trouv"))
			return ATScreenScraperOutcome::NotFound;
	}

	// ScreenScraper's published API v2 status table.  These are NOT the
	// conventional HTTP meanings — 429 is a thread-rate limit rather than
	// a quota, 430 is the daily quota, and 426 means this software build
	// has been blacklisted — so they are mapped explicitly.
	switch (httpStatus) {
		case 200:
			return ATScreenScraperOutcome::Matched;
		case 400:
			// Malformed request: bad CRC format, a path in romnom, or a
			// missing required field.  Our bug, not the user's, and
			// retrying unchanged cannot help — fail this entry only.
			return ATScreenScraperOutcome::TransientError;
		case 401:
			// "API closed for non-members or inactive members" — the
			// server is shedding load, not rejecting our credential.
			return ATScreenScraperOutcome::ApiClosed;
		case 403:
			return ATScreenScraperOutcome::BadCredentials;
		case 404:
			return ATScreenScraperOutcome::NotFound;
		case 423:
			return ATScreenScraperOutcome::ApiClosed;
		case 426:
			return ATScreenScraperOutcome::Blacklisted;
		case 429:
			return ATScreenScraperOutcome::RateLimited;
		case 430:
			return ATScreenScraperOutcome::QuotaExceeded;
		case 431:
			return ATScreenScraperOutcome::TooManyUnknownRoms;
		default:
			return ATScreenScraperOutcome::TransientError;
	}
}

// ---------------------------------------------------------------------------
// Response parsing
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// HTML entities
// ---------------------------------------------------------------------------

void ATScreenScraperDecodeEntities(VDStringW& text) {
	if (text.empty())
		return;
	// Nothing to do for the overwhelming majority of strings.
	if (text.find(L'&') == VDStringW::npos)
		return;

	struct Named { const wchar_t *name; wchar_t ch; };
	static const Named kNamed[] = {
		{ L"quot",   L'"'      },
		{ L"apos",   L'\''     },
		{ L"lt",     L'<'      },
		{ L"gt",     L'>'      },
		{ L"nbsp",   L' '      },
		{ L"eacute", L'\u00E9' },
		{ L"egrave", L'\u00E8' },
		{ L"agrave", L'\u00E0' },
		{ L"ccedil", L'\u00E7' },
		{ L"ocirc",  L'\u00F4' },
		{ L"ldquo",  L'"'      },
		{ L"rdquo",  L'"'      },
		{ L"lsquo",  L'\''     },
		{ L"rsquo",  L'\''     },
		{ L"ndash",  L'-'      },
		{ L"mdash",  L'-'      },
		{ L"hellip", L'\u2026' },
		// &amp; is decoded LAST by construction: it sits at the end of
		// this table and the scan is single-pass, so "&amp;quot;" comes
		// out as the literal text "&quot;" rather than being decoded
		// twice into a bare quote.
		{ L"amp",    L'&'      },
	};

	VDStringW out;
	out.reserve(text.size());

	const size_t n = text.size();
	for (size_t i = 0; i < n; ) {
		if (text[i] != L'&') {
			out += text[i++];
			continue;
		}

		// Find the terminating ';' within a sane distance.  An unmatched
		// '&' is just an ampersand and must survive untouched.
		size_t end = VDStringW::npos;
		for (size_t j = i + 1; j < n && j <= i + 10; ++j) {
			if (text[j] == L';') { end = j; break; }
			if (text[j] == L'&' || text[j] == L' ') break;
		}
		if (end == VDStringW::npos) {
			out += text[i++];
			continue;
		}

		const size_t bodyLen = end - i - 1;
		bool decoded = false;

		if (bodyLen >= 2 && text[i + 1] == L'#') {
			// Numeric: &#39; or &#x27;
			uint32 cp = 0;
			bool hex = (text[i + 2] == L'x' || text[i + 2] == L'X');
			size_t k = i + (hex ? 3 : 2);
			bool any = false;
			for (; k < end; ++k) {
				const wchar_t c = text[k];
				int d = -1;
				if (c >= L'0' && c <= L'9')      d = c - L'0';
				else if (hex && c >= L'a' && c <= L'f') d = 10 + (c - L'a');
				else if (hex && c >= L'A' && c <= L'F') d = 10 + (c - L'A');
				if (d < 0) { any = false; break; }
				cp = cp * (hex ? 16u : 10u) + (uint32)d;
				any = true;
				if (cp > 0x10FFFFu) { any = false; break; }
			}
			// Anything outside the BMP cannot be stored in a single
			// wchar_t on Windows, and none of this text needs it.
			if (any && cp >= 0x20 && cp <= 0xFFFF) {
				out += (wchar_t)cp;
				decoded = true;
			}
		} else if (bodyLen > 0) {
			for (const Named& e : kNamed) {
				const size_t len = wcslen(e.name);
				if (len != bodyLen)
					continue;
				if (wcsncmp(text.c_str() + i + 1, e.name, len) == 0) {
					out += e.ch;
					decoded = true;
					break;
				}
			}
		}

		if (decoded) {
			i = end + 1;
		} else {
			// Unrecognised: leave it exactly as it was.  Mangling text we
			// do not understand is worse than showing it.
			out += text[i++];
		}
	}

	text = out;
}

// ---------------------------------------------------------------------------
// Name matching
// ---------------------------------------------------------------------------

VDStringA ATScreenScraperCleanName(const char *fileName) {
	VDStringA out;
	if (!fileName)
		return out;

	// Drop the extension, but only a short trailing one — "M.U.L.E." must
	// not lose its final "E." to a naive last-dot rule.
	VDStringA base(fileName);
	size_t dot = VDStringA::npos;
	for (size_t i = base.size(); i-- > 0; ) {
		if (base[i] == '.') { dot = i; break; }
	}
	if (dot != VDStringA::npos && dot > 0 && base.size() - dot <= 5) {
		bool alnum = true;
		for (size_t i = dot + 1; i < base.size(); ++i) {
			const char c = base[i];
			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
				|| (c >= '0' && c <= '9')))
			{
				alnum = false;
				break;
			}
		}
		if (alnum)
			base.resize(dot);
	}

	// Strip bracketed groups.  On this platform they carry the year, the
	// publisher, the dumper, the cracking group, trainer counts and
	// "[!]"-style quality marks — none of which the database indexes as
	// part of the title, and all of which defeat an exact-name lookup.
	// Nesting is tracked rather than assumed: "(Rev A (PAL))" occurs.
	int depthParen = 0;
	int depthBracket = 0;
	for (size_t i = 0; i < base.size(); ++i) {
		const char c = base[i];
		if (c == '(')      { ++depthParen;   continue; }
		if (c == '[')      { ++depthBracket; continue; }
		if (c == ')')      { if (depthParen)   --depthParen;   continue; }
		if (c == ']')      { if (depthBracket) --depthBracket; continue; }
		if (depthParen || depthBracket)
			continue;

		// Separators that stand in for spaces in file names.  A dot is
		// NOT one of them: it is load-bearing in "M.U.L.E." and in
		// "Dr. Who", and dropping it would fold distinct titles together.
		out += (c == '_' || c == '-' || c == '+') ? ' ' : c;
	}

	// Collapse runs of whitespace and trim.
	VDStringA collapsed;
	bool pendingSpace = false;
	for (size_t i = 0; i < out.size(); ++i) {
		const char c = out[i];
		if (c == ' ' || c == '\t') {
			pendingSpace = !collapsed.empty();
			continue;
		}
		if (pendingSpace) {
			collapsed += ' ';
			pendingSpace = false;
		}
		collapsed += c;
	}
	return collapsed;
}

VDStringA ATScreenScraperMatchKey(const char *name) {
	VDStringA key;
	if (!name)
		return key;
	for (const char *p = name; *p; ++p) {
		char c = *p;
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			key += c;
	}
	return key;
}

int ATScreenScraperNameScore(const char *keyA, const char *keyB) {
	if (!keyA || !keyB || !*keyA || !*keyB)
		return 0;

	const size_t la = strlen(keyA);
	const size_t lb = strlen(keyB);
	if (la == lb && memcmp(keyA, keyB, la) == 0)
		return 100;

	// Containment, scaled by how much of the longer string is explained
	// by the shorter one.  "boulderdash" inside "boulderdashii" scores
	// 11/13 = 84, which lands just under the acceptance threshold — on
	// purpose.  A sequel is exactly the mistake this guard exists to stop.
	const char *shortS = la <= lb ? keyA : keyB;
	const char *longS  = la <= lb ? keyB : keyA;
	const size_t ls = la <= lb ? la : lb;
	const size_t ll = la <= lb ? lb : la;

	if (strstr(longS, shortS) != nullptr)
		return (int)((ls * 100) / ll);

	// No containment: fall back to a common-prefix measure, which is
	// weak on purpose.  Anything that needs this is unlikely to be a
	// safe automatic match.
	size_t common = 0;
	while (common < ls && shortS[common] == longS[common])
		++common;
	return (int)((common * 60) / ll);
}

const ATScreenScraperCandidate *ATScreenScraperPickBestCandidate(
	const std::vector<ATScreenScraperCandidate>& candidates,
	const char *wantedName, int& outScore)
{
	outScore = 0;
	const VDStringA wantKey = ATScreenScraperMatchKey(wantedName);
	if (wantKey.empty())
		return nullptr;

	const ATScreenScraperCandidate *best = nullptr;
	for (const auto& c : candidates) {
		const VDStringA key = ATScreenScraperMatchKey(c.mName.c_str());
		const int score = ATScreenScraperNameScore(wantKey.c_str(),
			key.c_str());
		if (score > outScore) {
			outScore = score;
			best = &c;
		}
	}

	if (!best || outScore < kATScreenScraperMinFuzzyScore)
		return nullptr;
	return best;
}

// ---------------------------------------------------------------------------
// Name search (jeuRecherche.php)
// ---------------------------------------------------------------------------

bool ATScreenScraperParseSearchResults(const void *json, size_t length,
	std::vector<ATScreenScraperCandidate>& outCandidates,
	VDStringA& outError)
{
	outCandidates.clear();
	outError.clear();

	if (!json || !length) {
		outError = "empty response";
		return false;
	}

	VDJSONDocument doc;
	VDJSONReader reader;
	if (!reader.Parse(json, length, doc)) {
		outError = "malformed JSON";
		return false;
	}

	auto root = doc.Root();
	if (!root.IsObject()) {
		outError = "unexpected response shape";
		return false;
	}
	auto response = root[L"response"];
	if (!response.IsObject()) {
		outError = "no response section";
		return false;
	}

	auto games = response[L"jeux"];
	if (!games.IsArray()) {
		// A search that matched nothing is a legitimate, non-error
		// answer — the caller distinguishes it by the empty list.
		return true;
	}

	for (const auto& game : games.AsArray()) {
		if (!game.IsObject())
			continue;

		ATScreenScraperCandidate cand;
		cand.mGameId = (uint32_t)ParseLeadingInt(PickText(game[L"id"]));
		if (!cand.mGameId)
			continue;

		// `noms` is the regional name list, same shape as jeuInfos.  Any
		// one of them is a valid thing for the user's file to have been
		// named after, so the scorer sees them all and keeps the best;
		// picking only "the" name would miss a US file matching a EU
		// title or vice versa.
		auto names = game[L"noms"];
		bool anyName = false;
		if (names.IsArray()) {
			for (const auto& n : names.AsArray()) {
				const wchar_t *text = PickText(n);
				if (text && *text) {
					ATScreenScraperCandidate byName;
					byName.mGameId = cand.mGameId;
					byName.mName = VDTextWToU8(VDStringSpanW(text));
					outCandidates.push_back(std::move(byName));
					anyName = true;
				}
			}
		}
		if (!anyName) {
			const wchar_t *nom = PickText(game[L"nom"]);
			if (nom && *nom) {
				cand.mName = VDTextWToU8(VDStringSpanW(nom));
				outCandidates.push_back(std::move(cand));
			}
		}
	}
	return true;
}

ATScreenScraperOutcome ATScreenScraperSearch(const VDStringA& name,
	int systemId, const ATMetadataSettings& settings,
	const std::atomic<bool> *cancel,
	std::vector<ATScreenScraperCandidate>& outCandidates,
	VDStringA& outError)
{
	outCandidates.clear();
	outError.clear();

	if (!ATHttp::Available())
		return ATScreenScraperOutcome::Unavailable;
	if (name.empty())
		return ATScreenScraperOutcome::NotFound;

	std::string url = kApiBase;
	url += "jeuRecherche.php?";
	if (!BuildAuthQuery(settings, url))
		return ATScreenScraperOutcome::NotConfigured;

	char sys[16];
	snprintf(sys, sizeof sys, "%d", systemId);
	AppendParam(url, "systemeid", sys);
	AppendParam(url, "recherche", name.c_str());

	ATHttp::Request req;
	req.url = std::move(url);
	req.timeoutMs = 20000;
	req.cancel = cancel;
	req.userAgent = UserAgent();
	req.maxBodyBytes = 4u * 1024u * 1024u;

	ATHttp::Response resp;
	ATHttp::Get(req, resp);

	if (resp.status == 200
		&& ATScreenScraperParseSearchResults(resp.body.data(),
			resp.body.size(), outCandidates, outError))
	{
		return outCandidates.empty()
			? ATScreenScraperOutcome::NotFound
			: ATScreenScraperOutcome::Matched;
	}

	// Same parse-before-classify rule as the game fetch: a successful
	// body can contain words the error classifier looks for.
	if (outError.empty())
		outError = "search failed";
	return ATScreenScraperClassifyError(resp.body.data(), resp.body.size(),
		resp.status);
}

bool ATScreenScraperParseGameInfo(
	const void *json, size_t length,
	const char *regionPref, const char *languagePref,
	ATScreenScraperResult& out, VDStringA& outError)
{
	out = ATScreenScraperResult();
	outError.clear();

	if (!json || !length) {
		outError = "empty response";
		return false;
	}

	VDJSONDocument doc;
	VDJSONReader reader;
	if (!reader.Parse(json, length, doc)) {
		outError = "malformed JSON";
		return false;
	}

	auto root = doc.Root();
	if (!root.IsObject()) {
		outError = "unexpected response shape";
		return false;
	}

	auto response = root[L"response"];
	if (!response.IsObject()) {
		outError = "no response section";
		return false;
	}

	// ssuser is present on every authenticated reply and tells us how
	// many parallel requests the account may make.
	auto ssuser = response[L"ssuser"];
	if (ssuser.IsObject()) {
		out.mMaxThreads = ParseLeadingInt(PickText(ssuser[L"maxthreads"]));
		out.mRequestsToday =
			ParseLeadingInt(PickText(ssuser[L"requeststoday"]));
		out.mMaxRequestsPerDay =
			ParseLeadingInt(PickText(ssuser[L"maxrequestsperday"]));
	}

	auto jeu = response[L"jeu"];
	if (!jeu.IsObject()) {
		outError = "no game in response";
		return false;
	}

	GameMetadata& m = out.mMeta;
	m.mProvider = "screenscraper";
	m.mStatus = GameMetaStatus::Matched;

	{
		const wchar_t *id = PickText(jeu[L"id"]);
		if (id)
			m.mProviderGameId = (uint32_t)ParseLeadingInt(id);
	}

	if (const wchar_t *name = PickLocalized(jeu[L"noms"], "region", regionPref))
		m.mTitle = name;

	if (const wchar_t *syn =
		PickLocalized(jeu[L"synopsis"], "langue", languagePref))
	{
		m.mDescription = syn;
	}

	if (const wchar_t *pub = PickText(jeu[L"editeur"]))
		m.mPublisher = pub;

	if (const wchar_t *dev = PickText(jeu[L"developpeur"]))
		m.mDeveloper = dev;

	// Provider text is HTML.  Decode it here, once, at the boundary —
	// every consumer downstream (both details panels, the library rows,
	// the cache) treats these as plain strings.
	ATScreenScraperDecodeEntities(m.mTitle);
	ATScreenScraperDecodeEntities(m.mDescription);
	ATScreenScraperDecodeEntities(m.mPublisher);
	ATScreenScraperDecodeEntities(m.mDeveloper);

	if (const wchar_t *players = PickText(jeu[L"joueurs"]))
		m.mPlayersMax = (uint8_t)ParseMaxPlayers(players);

	if (const wchar_t *note = PickText(jeu[L"note"])) {
		const int rating = ParseLeadingInt(note);
		m.mRating = (uint8_t)(rating < 0 ? 0 : (rating > 20 ? 20 : rating));
	}

	// Dates come back as "1984", "1984-01-01" or "01/1984"; the leading
	// four-digit run is the year in every form we have seen, except the
	// slash form, where we take the last four-digit run instead.
	if (const wchar_t *date = PickLocalized(jeu[L"dates"], "region", regionPref)) {
		int year = ParseLeadingInt(date);
		if (year < 1000 || year > 2999) {
			year = 0;
			for (const wchar_t *p = date; *p; ++p) {
				if (*p >= L'0' && *p <= L'9') {
					const int v = ParseLeadingInt(p);
					if (v >= 1000 && v <= 2999)
						year = v;
					while (*p >= L'0' && *p <= L'9')
						++p;
					if (!*p)
						break;
				}
			}
		}
		if (year >= 1000 && year <= 2999)
			m.mYear = (uint16_t)year;
	}

	// Genres: each entry carries its own localized name array.
	{
		auto genres = jeu[L"genres"];
		if (genres.IsArray()) {
			VDStringW joined;
			int count = 0;
			for (const auto& g : genres.AsArray()) {
				const wchar_t *name =
					PickLocalized(g[L"noms"], "langue", languagePref);
				if (!name || !*name)
					continue;
				if (!joined.empty())
					joined += L", ";
				joined += name;
				if (++count >= 4)   // keep it to one readable line
					break;
			}
			ATScreenScraperDecodeEntities(joined);
			m.mGenre = joined;
		}
	}

	if (regionPref)
		m.mRegion = VDTextAToW(regionPref);

	// Media.  Type preference first, region only as a tie-break — see
	// SelectMedia.
	{
		static const char *const kBoxTypes[] = {
			"box-2D", "box-2D-side", "box-3D", "flyer",
		};
		static const char *const kTitleTypes[]  = { "sstitle", "ss" };
		static const char *const kScreenTypes[] = { "ss", "sstitle" };
		static const char *const kLogoTypes[]   = {
			"wheel", "wheel-hd", "screenmarquee", "wheel-carbon",
		};

		const MediaPick picks[] = {
			{ kBoxTypes,    (int)(sizeof kBoxTypes / sizeof *kBoxTypes),
			  &out.mBoxArtUrl },
			{ kTitleTypes,  (int)(sizeof kTitleTypes / sizeof *kTitleTypes),
			  &out.mTitleShotUrl },
			{ kScreenTypes, (int)(sizeof kScreenTypes / sizeof *kScreenTypes),
			  &out.mScreenshotUrl },
			{ kLogoTypes,   (int)(sizeof kLogoTypes / sizeof *kLogoTypes),
			  &out.mLogoUrl },
		};

		SelectMedia(jeu[L"medias"], regionPref, picks,
			(int)(sizeof picks / sizeof *picks));

		// "ss" is both the screenshot and the title-screen fallback.
		// If both slots resolved to the same image, keep it only as the
		// screenshot so the details view does not show a duplicate.
		if (!out.mTitleShotUrl.empty()
			&& out.mTitleShotUrl == out.mScreenshotUrl)
		{
			out.mTitleShotUrl.clear();
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// Network entry points
// ---------------------------------------------------------------------------

ATScreenScraperOutcome ATScreenScraperFetch(
	const ATScreenScraperQuery& query,
	const ATMetadataSettings& settings,
	const std::atomic<bool>* cancel,
	ATScreenScraperResult& out,
	VDStringA& outError)
{
	out = ATScreenScraperResult();
	outError.clear();

	if (!ATMetadataNetworkAllowed(settings)) {
		outError = "Online metadata is disabled.";
		return ATScreenScraperOutcome::Unavailable;
	}
	if (!ATHttp::Available())
		return ATScreenScraperOutcome::Unavailable;

	std::string url = kApiBase;
	url += "jeuInfos.php?";
	if (!BuildAuthQuery(settings, url))
		return ATScreenScraperOutcome::NotConfigured;

	if (query.mPinnedGameId) {
		// A pinned id is authoritative — no hashes, no filename, no
		// chance of re-matching to a different game on a later run.
		AppendParamU64(url, "gameid", query.mPinnedGameId);
	} else {
		char sys[16];
		snprintf(sys, sizeof sys, "%d", query.mSystemId);
		AppendParam(url, "systemeid", sys);
		AppendParam(url, "romtype", "rom");

		if (!query.mbNameOnly && query.mCRC32) {
			char crc[16];
			snprintf(crc, sizeof crc, "%08X", (unsigned)query.mCRC32);
			AppendParam(url, "crc", crc);
		}
		if (!query.mRomName.empty())
			AppendParam(url, "romnom", query.mRomName.c_str());
		if (query.mFileSize)
			AppendParamU64(url, "romtaille", query.mFileSize);
	}

	ATHttp::Request req;
	req.url = std::move(url);
	req.timeoutMs = 20000;
	req.cancel = cancel;
	req.userAgent = UserAgent();
	// A jeuInfos reply is a few tens of KB at most; anything larger is
	// a sign something is wrong, not a big game record.
	req.maxBodyBytes = 4u * 1024u * 1024u;

	ATHttp::Response resp;
	ATHttp::Get(req, resp);

	if (resp.status == 0) {
		outError = resp.error.c_str();
		return ATScreenScraperOutcome::TransientError;
	}

	// Parse BEFORE classifying.  The classifier scans the body for the
	// provider's error strings, and a successful reply embeds the game's
	// synopsis — a description containing "quota" would otherwise abort
	// the whole run, and one containing "non trouv" would be recorded as
	// a miss.  A body that yields a game record is, by definition, not
	// an error.
	VDStringA parseError;
	if (resp.status == 200
		&& ATScreenScraperParseGameInfo(resp.body.data(), resp.body.size(),
			settings.mRegion.c_str(), settings.mLanguage.c_str(),
			out, parseError))
	{
		return ATScreenScraperOutcome::Matched;
	}

	const ATScreenScraperOutcome classified = ATScreenScraperClassifyError(
		resp.body.data(), resp.body.size(), resp.status);

	if (classified != ATScreenScraperOutcome::Matched) {
		if (classified == ATScreenScraperOutcome::NotFound)
			outError = "no match";
		else
			outError = ATScreenScraperOutcomeText(classified);
		return classified;
	}

	// HTTP 200, no error marker, but no usable game record either.  The
	// provider answers a miss this way on some code paths.
	if (parseError == "no game in response") {
		outError = "no match";
		return ATScreenScraperOutcome::NotFound;
	}

	outError = parseError.empty() ? VDStringA("unexpected response") : parseError;
	return ATScreenScraperOutcome::TransientError;
}

bool ATScreenScraperFetchAccount(
	const ATMetadataSettings& settings,
	const std::atomic<bool>* cancel,
	ATScreenScraperAccount& out)
{
	out = ATScreenScraperAccount();

	if (!ATMetadataNetworkAllowed(settings)) {
		out.mError = "Online metadata is disabled.";
		return false;
	}
	if (!ATHttp::Available()) {
		out.mError = ATScreenScraperOutcomeText(
			ATScreenScraperOutcome::Unavailable);
		return false;
	}

	std::string url = kApiBase;
	url += "ssuserInfos.php?";
	if (!BuildAuthQuery(settings, url)) {
		out.mError = ATScreenScraperOutcomeText(
			ATScreenScraperOutcome::NotConfigured);
		return false;
	}

	ATHttp::Request req;
	req.url = std::move(url);
	req.timeoutMs = 15000;
	req.cancel = cancel;
	req.userAgent = UserAgent();
	req.maxBodyBytes = 256u * 1024u;

	ATHttp::Response resp;
	ATHttp::Get(req, resp);

	if (resp.status == 0) {
		out.mError = resp.error.c_str();
		return false;
	}

	// Parse first, classify second — same reasoning as ATScreenScraperFetch.
	VDJSONDocument doc;
	VDJSONReader reader;
	const bool parsed = resp.status == 200
		&& reader.Parse(resp.body.data(), resp.body.size(), doc);

	VDJSONValueRef ssuser;
	bool haveAccount = false;
	if (parsed) {
		auto response = doc.Root()[L"response"];
		if (response.IsObject()) {
			ssuser = response[L"ssuser"];
			haveAccount = ssuser.IsObject();
		}
	}

	if (!haveAccount) {
		const ATScreenScraperOutcome classified = ATScreenScraperClassifyError(
			resp.body.data(), resp.body.size(), resp.status);
		if (classified != ATScreenScraperOutcome::Matched) {
			const char *text = ATScreenScraperOutcomeText(classified);
			out.mError = (text && *text)
				? text : "ScreenScraper rejected the request.";
		} else {
			out.mError = parsed
				? "no account in response" : "malformed response";
		}
		return false;
	}

	out.mbValid = true;
	if (const wchar_t *level = PickText(ssuser[L"niveau"]))
		out.mLevel = VDTextWToA(VDStringW(level));
	out.mMaxThreads = ParseLeadingInt(PickText(ssuser[L"maxthreads"]));
	out.mRequestsToday = ParseLeadingInt(PickText(ssuser[L"requeststoday"]));
	out.mMaxRequestsPerDay =
		ParseLeadingInt(PickText(ssuser[L"maxrequestsperday"]));
	return true;
}
