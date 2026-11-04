// banana-tube: a light YouTube search-and-play client for the Retroid Pocket Classic (SM4450). One SDL2 window
// under cage (the kiosk compositor turns it with the panel, as the ports and RetroArch run); a search hits YouTube's
// InnerTube API directly (with yt-dlp as a fallback), thumbnails are cached under /data/banana/cache/tube, and the
// chosen video plays in an mpv child process driven over its JSON IPC socket. No browser, no Electron, no web view.
//
//   banana-tube [--font FILE]
//
// Controls, on the RPC's own pad (the launcher exports SDL's mapping for it):
//   d-pad          move on the keyboard / through the results / seek while playing
//   A              type the key, play the chosen video, play/pause while playing
//   B              back a step (playing -> results -> keyboard -> quit), clear the query
//   X / Y          backspace / shift (keyboard), previous / next page (results)
//   L / R          a page up / down through the results
//   START          run the search (keyboard) / a new search (results)
//
// Playback choices that matter on this SoC (measured on the unit):
//   * Codec: YouTube offers AV1 first and the a613 has no AV1 decoder, so mpv rendered no frames. The format chain
//     prefers H.264 (avc1) then VP9 and never av01.
//   * Output: --vo=wlshm (software, shared memory). mpv's GPU output under cage renders a white frame
//     intermittently here; the software output is stable and draws like banana-ui's own panel.
//   * Decode: --hwdec=auto-copy (a copy-back hwdec like v4l2m2m-copy), so a supported codec decodes on the GPU and
//     the frames come back to system memory for wlshm; it falls back to software on its own.
//   * Audio: the second AudioReach frontend (MultiMedia2, hw:0,1) via the launcher's private $HOME/.asoundrc, S16,
//     with an mpv audio buffer matched to the tiny AudioReach period (see run-bananatube).

#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL_image.h>
#include <curl/curl.h>

#include "json.h"

#include <atomic>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <linux/input.h>

// mpv's JSON IPC socket (banana-tube drives playback over it): a macro so it concatenates into mpv's argv
#define IPC_SOCK "/run/banana/bananatube.sock"
// on the card, beside settings.conf: the last videos played (Select on the keyboard shows them) and the playback
// quality, so both survive a reboot
#define RECENT_FILE "/data/banana/bananatube-recent.tsv"
#define CONF_FILE "/data/banana/bananatube.conf"

// A stop asked from outside (the launcher's `kill` on the app's process group, a shutdown): the loop leaves and the
// mpv child is reaped, so the menu's launcher always returns and the menu comes back.
static volatile sig_atomic_t g_stop = 0;
static void onStopSignal(int) { g_stop = 1; }

namespace
{
	const char* DEFAULT_FONT = "/usr/share/banana/rnds/fonts/PixelifySans-Regular.ttf";
	const char* DEFAULT_FONT_MED = "/usr/share/banana/rnds/fonts/PixelifySans-Medium.ttf";
	const char* FALLBACK_FONT = "/usr/share/banana/banana/fonts/Inter-Regular.ttf";
	const char* FALLBACK_FONT_MED = "/usr/share/banana/banana/fonts/Inter-SemiBold.ttf";
	// the theme's pixel faces, or Inter when this is not a Banana image (the host selftest)
	TTF_Font* openFont(const std::string& path, int px, const char* fallback)
	{
		TTF_Font* f = TTF_OpenFont(path.c_str(), px);
		if (!f && fallback) f = TTF_OpenFont(fallback, px);
		return f;
	}
	const char* CACHE_DIR = "/data/banana/cache/tube";
	// YouTube's public web InnerTube key (the one its own web client ships): the search endpoint needs it. It is not
	// a secret and is not per-user; a failure just falls back to yt-dlp.
	const char* INNERTUBE_KEY = "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8";
	const char* INNERTUBE_SEARCH = "https://www.youtube.com/youtubei/v1/search";

	enum class Mode { KEYS, RESULTS, PLAYING };
	enum class View { SEARCH, RECENT, CHANNELS, LOCAL };

	// a SponsorBlock segment of the video playing: skipped (seeked past) once per playback
	struct Skip { double s, e; std::string cat; bool done; };

	struct Item
	{
		std::string id, title, uploader, duration;
		double resume = 0;		// where playback stopped, in seconds (0 = start; recent only)
		bool local = false;		// id is a file on the card (the Local tab), not a YouTube id
	};

	// the on-screen keyboard: rows of keys, the last row the actions. A key is one UTF-8 string ("Shift" swaps the
	// letters' case, so one list does both).
	const char* LOWER[][11] = {
		{ "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "" },
		{ "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "" },
		{ "a", "s", "d", "f", "g", "h", "j", "k", "l", "-", "" },
		{ "z", "x", "c", "v", "b", "n", "m", ",", ".", "'", "" },
	};
	const char* UPPER[][11] = {
		{ "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "" },
		{ "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "" },
		{ "A", "S", "D", "F", "G", "H", "J", "K", "L", "_", "" },
		{ "Z", "X", "C", "V", "B", "N", "M", ";", ":", "\"", "" },
	};
	const int ROWS = 4, COLS = 10;
	// the action row's button a sits under key column (a + 0.5) * COLS / 4, and a key column c is under the button
	// whose span holds its centre; the two agree, so up and down come back to the same column
	int actToKey(int a) { return std::min(COLS - 1, (2 * a + 1) * COLS / 8); }
	int keyToAct(int c) { return std::min(3, (2 * c + 1) * 4 / (2 * COLS)); }

	std::string shellQuote(const std::string& s)
	{
		std::string r = "'";
		for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
		return r + "'";
	}

	// a URL-safe file name for a video id (ids are already [A-Za-z0-9_-], but a JSON field is not trusted)
	std::string cacheName(const std::string& id)
	{
		std::string r;
		for (char c : id) r += (isalnum((unsigned char)c) || c == '_' || c == '-') ? c : '_';
		return r;
	}

	// ---- HTTP (libcurl) ------------------------------------------------------------------------------------------
	size_t curlWrite(char* p, size_t sz, size_t n, void* u)
	{
		((std::string*)u)->append(p, sz * n);
		return sz * n;
	}
	std::string curlBody(const std::string& url, const std::string* post, bool* ok, long timeoutSec = 15)
	{
		std::string buf;
		if (ok) *ok = false;
		CURL* c = curl_easy_init();
		if (!c) return buf;
		curl_easy_setopt(c, CURLOPT_URL, url.c_str());
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWrite);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
		curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
		curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSec);
		curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
		curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux aarch64) BananaTube/1");
		struct curl_slist* h = nullptr;
		if (post)
		{
			curl_easy_setopt(c, CURLOPT_POSTFIELDS, post->c_str());
			curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)post->size());
			h = curl_slist_append(h, "Content-Type: application/json");
			curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
		}
		CURLcode r = curl_easy_perform(c);
		long code = 0;
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
		if (h) curl_slist_free_all(h);
		curl_easy_cleanup(c);
		if (ok) *ok = (r == CURLE_OK && code >= 200 && code < 300);
		return buf;
	}
	// a small JSON string escape for the request body's query
	std::string jsonEsc(const std::string& s)
	{
		std::string r;
		for (char c : s)
		{
			if (c == '"' || c == '\\') { r += '\\'; r += c; }
			else if (c == '\n') r += "\\n";
			else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); r += b; }
			else r += c;
		}
		return r;
	}

	// ---- InnerTube search ----------------------------------------------------------------------------------------
	// The public web search endpoint. The result's shape changes over time, so the walk is defensive: the first
	// videoRenderer block found (through whatever section wrapper) wins, and a shape miss yields an empty list (the
	// caller falls back to yt-dlp). videoId, title, ownerText and lengthText are the stable fields.
	const banana::JVal* objAt(const banana::JVal* v, const char* k) { return v ? v->get(k) : nullptr; }
	std::string runText(const banana::JVal* runs)
	{
		if (!runs) return "";
		if (runs->isStr()) return runs->s;	// a bare string
		return runs->arr.empty() ? "" : runs->arr[0]->str("text");
	}
	void collectRenderers(const banana::JVal* node, std::vector<Item>& out, int depth = 0)
	{
		if (!node || depth > 12) return;
		if (node->isObj())
		{
			if (const banana::JVal* vr = node->get("videoRenderer"))
			{
				Item it;
				it.id = vr->str("videoId");
				const banana::JVal* t = vr->get("title");
				it.title = t ? runText(t->get("runs")) : "";
				if (it.title.empty() && t) it.title = t->str("simpleText");
				it.uploader = runText(objAt(objAt(vr, "ownerText"), "runs"));
				if (it.uploader.empty())
				{
					const banana::JVal* ch = objAt(objAt(vr, "longBylineText"), "runs");
					it.uploader = runText(ch);
				}
				it.duration = objAt(vr, "lengthText") ? objAt(vr, "lengthText")->str("simpleText") : "";
				if (!it.id.empty() && !it.title.empty()) out.push_back(it);
			}
			for (auto& m : node->obj) collectRenderers(m.second.get(), out, depth + 1);
		}
		else if (node->isArr())
		{
			for (auto& e : node->arr) collectRenderers(e.get(), out, depth + 1);
		}
	}
	std::vector<Item> innerTubeSearch(const std::string& q, bool* ok)
	{
		std::vector<Item> out;
		if (ok) *ok = false;
		std::string body = "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"2.20240101.00.00\","
			"\"hl\":\"en\",\"gl\":\"US\"}},\"query\":\"" + jsonEsc(q) + "\",\"params\":\"\"}";
		std::string url = std::string(INNERTUBE_SEARCH) + "?key=" + INNERTUBE_KEY + "&prettyPrint=false";
		bool httpOk = false;
		std::string resp = curlBody(url, &body, &httpOk, 12);
		if (!httpOk || resp.empty()) return out;
		banana::JPtr doc = banana::parseJson(resp);
		if (!doc) return out;
		collectRenderers(doc.get(), out);
		// the same video can appear in the results and in a "related" shelf: keep the first of each id
		{
			std::set<std::string> seen;
			std::vector<Item> uniq;
			for (auto& it : out) if (seen.insert(it.id).second) uniq.push_back(it);
			out.swap(uniq);
		}
		if (out.size() > 40) out.resize(40);
		if (ok) *ok = true;
		return out;
	}

	// ---- yt-dlp fallback (the original path) ---------------------------------------------------------------------
	bool splitLine(const std::string& ln, Item& it)
	{
		size_t a = ln.find('\t');
		if (a == std::string::npos) return false;
		size_t b = ln.find('\t', a + 1);
		if (b == std::string::npos) return false;
		size_t c = ln.find('\t', b + 1);
		if (c == std::string::npos) return false;
		it.id = ln.substr(0, a);
		it.title = ln.substr(a + 1, b - a - 1);
		it.uploader = ln.substr(b + 1, c - b - 1);
		it.duration = ln.substr(c + 1);
		return !it.id.empty();
	}
	std::vector<Item> ytdlpSearch(const std::string& q)
	{
		std::string cmd = "yt-dlp --flat-playlist --no-warnings --ignore-errors "
			"--print \"%(id)s\t%(title)s\t%(uploader)s\t%(duration)s\" "
			"--cache-dir /data/banana/bananatube " + shellQuote("ytsearch24:" + q) + " 2>/dev/null";
		std::vector<Item> got;
		FILE* p = popen(cmd.c_str(), "r");
		if (!p) return got;
		char buf[4096];
		while (fgets(buf, sizeof buf, p))
		{
			std::string ln = buf;
			while (!ln.empty() && (ln.back() == '\n' || ln.back() == '\r')) ln.pop_back();
			Item it;
			if (splitLine(ln, it)) got.push_back(it);
		}
		pclose(p);
		return got;
	}
}

struct App
{
	SDL_Window* win = nullptr;
	SDL_Renderer* ren = nullptr;
	TTF_Font* fBig = nullptr;
	TTF_Font* fMed = nullptr;
	TTF_Font* fSmall = nullptr;
	SDL_GameController* pad = nullptr;
	std::string fontPath = DEFAULT_FONT;
	std::string fontPathMed = DEFAULT_FONT_MED;

	// The Pocket palette (system/rootfs/usr/share/banana/themes/pocket.json), one place for every draw function.
	// Gold is focus/active/brand only; the ground, cards and text stay brown-black, butter and olive.
	struct Skin
	{
		SDL_Color bg, bezel, ring, panel, card, line, edge, ink, soft, gold, goldHi, dot;
		int bw;
	};
	static Skin skin(int W)
	{
		int bw = std::max(2, W / 310);
		return { { 0x17, 0x14, 0x0d, 255 }, { 0x0b, 0x09, 0x06, 255 }, { 0x34, 0x2c, 0x19, 255 },
			{ 0x20, 0x1b, 0x11, 255 }, { 0x2a, 0x23, 0x15, 255 }, { 0x4a, 0x3d, 0x18, 255 },
			{ 0x8c, 0x73, 0x28, 255 }, { 0xff, 0xef, 0xb2, 255 }, { 0xbf, 0xa9, 0x63, 255 },
			{ 0xff, 0xd2, 0x1e, 255 }, { 0xff, 0xe8, 0x73, 255 }, { 0x24, 0x1f, 0x14, 255 }, bw };
	}
	SDL_Texture* backdrop = nullptr;	// the frame and dot grid, baked once per output size
	int backdropW = 0, backdropH = 0;
	SDL_Texture* logo = nullptr;		// the OS banana mark beside the wordmark

	Mode mode = Mode::KEYS;
	int keyRow = 1, keyCol = 0;
	bool upper = false;
	std::string query;
	std::string status;
	std::vector<Item> results;
	int sel = 0, scroll = 0;
	bool dirty = true;					// draw only when the frame can have changed (see markDirty())
	void markDirty() { dirty = true; }
	bool quit = false;					// the loop's own exit (B on an empty query): SDL_Quit() here would tear SDL
										// down under a running loop, leaving a frozen/glitched window that never exits

	pid_t mpv = -1;
	int ipc = -1;
	bool paused = false;				// the play/pause the indicators show (toggled with the command we send)
	bool stalled = false, stallShown = false;	// mpv is waiting on the network: a buffering indicator
	int bufPct = 100;					// cache-buffering-state (0..100)
	Uint32 lastBufAsk = 0;				// when the two buffering properties were last asked
	Item playingItem;					// what Y reloads at another quality
	std::vector<Item> recent;			// the last videos played, newest first (recentAdd)
	std::vector<Item> searchResults;	// the last search's hits, kept while another tab is up
	std::vector<std::string> channels;	// the unique uploaders of those two lists (Channels tab)
	std::vector<Item> localItems;		// the music and video folders' files (Local tab)
	View view = View::SEARCH;			// the active tab: Search, Recent, Channels
	bool channelOpen = false;			// the Channels tab is showing one channel's videos
	std::string channelName;			// that channel's name
	int quality = 720;					// the playback quality cap in pixels (cycleQuality)
	std::string playingId;				// the video mpv is on, so its resume position follows the right row
	double pendingSeek = 0;				// where the video should resume, applied once the IPC socket is up
	Uint32 lastResumeSave = 0;			// when the position was last written to the recent file
	std::vector<Skip> segs;				// the playing video's SponsorBlock segments (sbFetch)
	int lastSkip = -1;					// the segment skipped last, for X (sbUndo)
	std::string ipcBuf;					// mpv's replies, not yet a whole line
	double pos = 0;						// mpv's time-pos, from the last reply to POS_REQ
	double dur = 0;						// mpv's duration, from the last reply to DUR_REQ
	bool havePos = false;
	Uint32 lastAsk = 0;
	long lastDurAsk = 0;
	static constexpr long POS_REQ = 7;	// request_id of the time-pos question
	static constexpr long DUR_REQ = 8;	// request_id of the duration question
	static constexpr long BUF_REQ = 9;	// paused-for-cache
	static constexpr long CACHE_REQ = 10;	// cache-buffering-state
	static std::string fmtTime(double t)
	{
		int s = static_cast<int>(t < 0 ? 0 : t);
		char b[32];
		snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
		return b;
	}
	std::string timeTag() const
	{
		if (!havePos || dur <= 0) return "";
		return fmtTime(pos) + " / " + fmtTime(dur);
	}
	// A text bar for mpv's OSD (the app cannot draw over the video): | where playback is, - around it.
	static std::string barText(double at, double total, int cells = 12)
	{
		if (total <= 0) return std::string(cells, '-');
		int k = (int)(at / total * cells);
		if (k < 0) k = 0;
		if (k > cells - 1) k = cells - 1;
		std::string s;
		for (int i = 0; i < cells; i++) s += i == k ? '|' : '-';
		return s;
	}
	static int nextQuality(int q) { return q == 720 ? 480 : q == 480 ? 360 : 720; }
	// "03:21 / 12:04" with the bar under it; the empty readout before mpv knows the length
	std::string whereTag(double at) const
	{
		if (dur <= 0) return fmtTime(at > 0 ? at : 0);
		double t = at < 0 ? 0 : at > dur ? dur : at;
		return fmtTime(t) + " / " + fmtTime(dur) + "\n" + barText(t, dur);
	}
	// the same for a seek: say where it landed (the polled position is the base for the target)
	void seekInd(const char* label, double delta)
	{
		double target = dur > 0 ? std::min(dur, std::max(0.0, pos + delta)) : pos + delta;
		showInd(std::string(label) + "  " + whereTag(target), 900);
	}

	std::mutex searchLock;
	std::thread searchThread;
	std::atomic<bool> searching { false };
	std::atomic<bool> searchDone { false };
	std::vector<Item> found;
	std::string searchErr;

	// thumbnails: downloaded to disk by one background worker, loaded into textures on the render thread, keyed by id
	std::map<std::string, SDL_Texture*> thumbTex;
	int thumbWinFirst = -1, thumbWinLast = -1;	// the visible window thumbTex was last pruned to
	std::thread thumbThread;
	std::mutex thumbMx;
	std::condition_variable thumbCv;
	std::atomic<bool> thumbPending { false };
	std::atomic<unsigned> thumbGeneration { 0 };	// a picture landed: the loop redraws the row that was waiting
	bool thumbStop = false;

	// ---- text ----------------------------------------------------------------------------------------------------
	// A rendered string is owned here: the font pointer + colour + text is the key, so a repeated row costs a map
	// lookup and a copy, not a TTF rasterisation and a texture. Bounded, and evicted all at once (a big change of
	// screen is the only time it fills).
	struct TextEntry { SDL_Texture* t = nullptr; int w = 0, h = 0; };
	std::map<const void*, std::map<Uint32, std::map<std::string, TextEntry>>> textCache;
	size_t textHits = 0, textMisses = 0, textCacheCount = 0;
	void clearTextCache()
	{
		for (auto& f : textCache)
			for (auto& c : f.second)
				for (auto& e : c.second)
					if (e.second.t) SDL_DestroyTexture(e.second.t);
		textCache.clear();
		textCacheCount = 0;
	}
	SDL_Texture* cachedText(const std::string& s, TTF_Font* f, SDL_Color c, int* w, int* h)
	{
		Uint32 rgba = (Uint32)c.r | ((Uint32)c.g << 8) | ((Uint32)c.b << 16) | ((Uint32)c.a << 24);
		auto& byColour = textCache[f];
		auto ci = byColour.find(rgba);
		if (ci != byColour.end())
		{
			auto e = ci->second.find(s);	// the caller's string: a hit copies and allocates nothing
			if (e != ci->second.end()) { textHits++; if (w) *w = e->second.w; if (h) *h = e->second.h; return e->second.t; }
		}
		SDL_Texture* t = text(s, f, c, w, h);
		if (!t) return nullptr;
		if (textCacheCount >= 256) clearTextCache();
		textCache[f][rgba][s] = TextEntry { t, w ? *w : 0, h ? *h : 0 };
		textCacheCount++;
		textMisses++;
		return t;
	}
	SDL_Texture* text(const std::string& s, TTF_Font* f, SDL_Color col, int* w = nullptr, int* h = nullptr)
	{
		if (s.empty()) { if (w) *w = 0; if (h) *h = 0; return nullptr; }
		SDL_Surface* surf = TTF_RenderUTF8_Blended(f, s.c_str(), col);
		if (!surf) { if (w) *w = 0; if (h) *h = 0; return nullptr; }
		SDL_Texture* t = SDL_CreateTextureFromSurface(ren, surf);
		if (w) *w = surf->w;
		if (h) *h = surf->h;
		SDL_FreeSurface(surf);
		return t;
	}
	void drawText(const std::string& s, TTF_Font* f, SDL_Color col, int x, int y, int maxW = 0)
	{
		if (s.empty()) return;
		std::string shown = s;
		if (maxW > 0)
		{
			int tw = 0, th = 0;
			TTF_SizeUTF8(f, shown.c_str(), &tw, &th);
			while (tw > maxW && shown.size() > 1)
			{
				shown.resize(shown.size() - 1);
				while (!shown.empty() && (static_cast<unsigned char>(shown.back()) & 0xC0) == 0x80) shown.pop_back();
				TTF_SizeUTF8(f, shown.c_str(), &tw, &th);
			}
			if (tw > maxW && shown.size() > 1) shown += "\u2026";
		}
		int w = 0, h = 0;
		SDL_Texture* t = cachedText(shown, f, col, &w, &h);
		if (!t) return;
		SDL_Rect r { x, y, w, h };
		SDL_RenderCopy(ren, t, nullptr, &r);
	}
	// a rounded-ish focus panel: a filled rect with a 2px accent ring
	void panel(int x, int y, int w, int h, SDL_Color fill, SDL_Color line, int ring = 0)
	{
		SDL_SetRenderDrawColor(ren, fill.r, fill.g, fill.b, fill.a);
		SDL_Rect r { x, y, w, h };
		SDL_RenderFillRect(ren, &r);
		for (int i = 0; i < (ring > 0 ? 2 : 1); i++)
		{
			SDL_SetRenderDrawColor(ren, line.r, line.g, line.b, line.a);
			SDL_Rect o { x - i, y - i, w + 2 * i, h + 2 * i };
			SDL_RenderDrawRect(ren, &o);
		}
	}
	// the Pocket surface: a hard fill with a solid edge of `border` pixels (the theme draws no rounded corners)
	void box(int x, int y, int w, int h, SDL_Color fill, SDL_Color edge, int border)
	{
		if (fill.a)
		{
			SDL_SetRenderDrawColor(ren, fill.r, fill.g, fill.b, fill.a);
			SDL_Rect r { x, y, w, h };
			SDL_RenderFillRect(ren, &r);
		}
		if (border <= 0 || edge.a == 0) return;
		SDL_SetRenderDrawColor(ren, edge.r, edge.g, edge.b, edge.a);
		SDL_Rect t { x, y, w, border }; SDL_RenderFillRect(ren, &t);
		t = { x, y + h - border, w, border }; SDL_RenderFillRect(ren, &t);
		t = { x, y, border, h }; SDL_RenderFillRect(ren, &t);
		t = { x + w - border, y, border, h }; SDL_RenderFillRect(ren, &t);
	}

	// ---- thumbnails ----------------------------------------------------------------------------------------------
	std::string thumbPath(const std::string& id) { return std::string(CACHE_DIR) + "/" + cacheName(id) + ".jpg"; }
	SDL_Texture* thumb(const std::string& id)
	{
		auto it = thumbTex.find(id);
		if (it != thumbTex.end()) return it->second;
		std::string p = thumbPath(id);
		struct stat st;
		if (stat(p.c_str(), &st) || st.st_size < 100) return nullptr;	// not downloaded yet (it is re-checked each draw)
		SDL_Surface* s = IMG_Load(p.c_str());
		if (!s) return nullptr;
		SDL_Texture* t = SDL_CreateTextureFromSurface(ren, s);
		SDL_FreeSurface(s);
		thumbTex[id] = t;
		return t;
	}
	// the one thumbnail worker: it waits for a request, then downloads whatever is missing for the current results. A
	// new request (a new search) is seen as thumbPending and cuts the current pass short, so it never blocks the UI.
	void thumbWorkerLoop()
	{
		for (;;)
		{
			{
				std::unique_lock<std::mutex> l(thumbMx);
				thumbCv.wait(l, [&] { return thumbStop || thumbPending.load(); });
				if (thumbStop) return;
				thumbPending = false;
			}
			std::vector<Item> list;
			{ std::lock_guard<std::mutex> l(searchLock); list = results; }
			for (auto& it : list)
			{
				if (thumbStop || thumbPending.load()) break;
				if (it.local) continue;	// a file on the card has no i.ytimg.com thumbnail
				std::string p = thumbPath(it.id);
				struct stat st;
				if (!stat(p.c_str(), &st) && st.st_size > 100) continue;
				std::string url = "https://i.ytimg.com/vi/" + it.id + "/mqdefault.jpg";
				bool ok = false;
				std::string bytes = curlBody(url, nullptr, &ok, 12);
				if (!ok || bytes.size() < 100) continue;
				std::string tmp = p + ".part";
				if (FILE* f = fopen(tmp.c_str(), "wb")) { fwrite(bytes.data(), 1, bytes.size(), f); fclose(f); rename(tmp.c_str(), p.c_str()); thumbGeneration++; }
			}
		}
	}
	void prefetchThumbs()
	{
		{ std::lock_guard<std::mutex> l(thumbMx); thumbPending = true; }
		thumbCv.notify_one();
	}

	// ---- search --------------------------------------------------------------------------------------------------
	void startSearch()
	{
		markDirty();	// "Searching…"
		if (query.empty() || searching) return;
		searchDone = false;
		searching = true;
		status = "Searching\u2026";
		view = View::SEARCH;
		channelOpen = false;
		results.clear();
		sel = scroll = 0;
		std::string q = query;
		if (searchThread.joinable()) searchThread.join();
		searchThread = std::thread([this, q]
		{
			bool apiOk = false;
			std::vector<Item> got = innerTubeSearch(q, &apiOk);
			std::string err;
			if (got.empty())
			{
				// InnerTube gave nothing (shape change, no network route to it): the yt-dlp path still works
				got = ytdlpSearch(q);
				if (got.empty()) err = "No results (network?)";
			}
			{
				std::lock_guard<std::mutex> l(searchLock);
				found = std::move(got);
				searchErr = err;
			}
			searchDone = true;
		});
	}
	void pollSearch()
	{
		if (!searchDone.exchange(false)) return;
		markDirty();
		if (searchThread.joinable()) searchThread.join();
		std::lock_guard<std::mutex> l(searchLock);
		searching = false;
		searchResults = found;
		buildChannels();		// the tab strip's Channels count follows the new hits
		if (view == View::SEARCH)
		{
			results = found;
			sel = scroll = 0;
			if (results.empty()) status = searchErr.empty() ? "No results" : searchErr;
			else status = std::to_string(results.size()) + " results   SEL " + std::to_string(quality) + "p";
			if (!results.empty())
			{
				mode = Mode::RESULTS;
				for (auto& kv : thumbTex) if (kv.second) SDL_DestroyTexture(kv.second);
				thumbTex.clear();
				prefetchThumbs();
			}
		}
	}

	// ---- recent & quality ----------------------------------------------------------------------------------------
	// a field of the recent file is one line: a tab or a newline in a title would split the record
	static std::string cleanField(const std::string& s)
	{
		std::string r = s;
		for (char& c : r) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
		return r;
	}
	// one recent line -> an Item (5th field optional: the resume position older files do not carry)
	static bool parseRecentLine(const std::string& s, Item& it)
	{
		std::vector<std::string> f;
		size_t p = 0;
		while (p <= s.size())
		{
			size_t t = s.find('\t', p);
			if (t == std::string::npos) { f.push_back(s.substr(p)); break; }
			f.push_back(s.substr(p, t - p));
			p = t + 1;
		}
		if (f.size() < 4 || f[0].empty()) return false;
		it = Item { f[0], f[1], f[2], f[3] };
		if (f.size() >= 5) it.resume = strtod(f[4].c_str(), nullptr);
		return true;
	}
	void recentLoad()
	{
		FILE* f = fopen(RECENT_FILE, "r");
		if (!f) return;
		char line[2048];
		while (fgets(line, sizeof line, f))
		{
			std::string s = line;
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
			Item it;
			if (!s.empty() && parseRecentLine(s, it)) recent.push_back(it);
		}
		fclose(f);
	}
	void recentSave()
	{
		/* temp + rename: the file is rewritten on every play and every progress save, and a card that fills up (or a
		 * power cut) in the middle used to leave it short, losing the history it held (library.cpp's writeAtomic) */
		const char* tmp = RECENT_FILE ".new";
		FILE* f = fopen(tmp, "w");
		if (!f) return;
		for (const Item& it : recent)
			fprintf(f, "%s\t%s\t%s\t%s\t%.1f\n", cleanField(it.id).c_str(), cleanField(it.title).c_str(),
				cleanField(it.uploader).c_str(), cleanField(it.duration).c_str(), it.resume);
		fflush(f);
		fsync(fileno(f));
		bool ok = !ferror(f) && fclose(f) == 0;
		if (ok) rename(tmp, RECENT_FILE);
		else unlink(tmp);
	}
	// the newest first, one entry per video, a bounded list
	void recentAdd(const Item& it)
	{
		Item add = it;
		// re-opening a video from Search must not wipe the position held by its Recent row
		if (add.resume <= 0)
			for (const Item& o : recent) if (o.id == it.id) { add.resume = o.resume; break; }
		std::vector<Item> keep;
		for (const Item& o : recent) if (o.id != it.id && keep.size() < 29) keep.push_back(o);
		recent.clear();
		recent.push_back(add);
		for (const Item& o : keep) recent.push_back(o);
		recentSave();
		buildChannels();
	}
	// the playing video's position into its recent row; the end of a video (or the last 15 s) clears it so the next
	// play starts over instead of resuming at the credits
	void saveProgress(bool finished = false)
	{
		if (playingId.empty()) return;
		for (Item& it : recent)
			if (it.id == playingId)
			{
				double p = (finished || (dur > 0 && pos > dur - 15)) ? 0 : pos;
				if (p < 5) p = 0;
				if (it.resume == p) return;
				it.resume = p;
				recentSave();
				return;
			}
	}
	void resetThumbs()
	{
		for (auto& kv : thumbTex) if (kv.second) SDL_DestroyTexture(kv.second);
		thumbTex.clear();
		thumbWinFirst = thumbWinLast = -1;
		prefetchThumbs();
	}
	// Keep only the visible page's textures (+/- one row): a long browse would otherwise hold every row's picture
	// for the life of the list. Runs when the window moves, so the render loop still allocates nothing.
	void pruneThumbs(int first, int last)
	{
		if (first < 0) first = 0;
		if (last > static_cast<int>(results.size())) last = static_cast<int>(results.size());
		for (auto it = thumbTex.begin(); it != thumbTex.end(); )
		{
			bool keep = false;
			for (int i = first; i < last && !keep; i++) keep = results[i].id == it->first;
			if (keep) ++it;
			else { if (it->second) SDL_DestroyTexture(it->second); it = thumbTex.erase(it); }
		}
		thumbWinFirst = first;
		thumbWinLast = last;
	}
	// the three tabs of the results shell: the search's hits, everything played (persisted on the card), and the
	// channels those two lists mention
	int listSize() const { return view == View::CHANNELS && !channelOpen ? static_cast<int>(channels.size())
		: static_cast<int>(results.size()); }
	void buildChannels()
	{
		channels.clear();
		auto add = [this](const Item& it)
		{
			if (it.uploader.empty()) return;
			for (const std::string& c : channels) if (c == it.uploader) return;
			channels.push_back(it.uploader);
		};
		for (const Item& it : searchResults) add(it);
		for (const Item& it : recent) add(it);
	}
	void showChannels()
	{
		view = View::CHANNELS;
		channelOpen = false;
		buildChannels();
		results.clear();
		sel = scroll = 0;
		mode = Mode::RESULTS;	// an empty list still shows as the tab, with its own hint
		status = channels.empty() ? "No channels yet. Search or play something."
			: std::to_string(channels.size()) + " channels   A opens one";
		resetThumbs();
	}
	void openChannel(const std::string& name)
	{
		view = View::CHANNELS;
		channelOpen = true;
		channelName = name;
		results.clear();
		auto add = [this, &name](const Item& it)
		{
			if (it.uploader != name) return;
			for (const Item& o : results) if (o.id == it.id) return;
			results.push_back(it);
		};
		for (const Item& it : searchResults) add(it);
		for (const Item& it : recent) add(it);
		sel = scroll = 0;
		mode = Mode::RESULTS;
		status = "Channel: " + name + "   " + std::to_string(results.size()) + " videos   B back";
		resetThumbs();
	}
	void showRecent()
	{
		view = View::RECENT;
		channelOpen = false;
		results = recent;
		sel = scroll = 0;
		mode = Mode::RESULTS;	// an empty list still shows as the tab, with its own hint
		status = "Recent   SEL " + std::to_string(quality) + "p";
		resetThumbs();
	}
	void showSearchTab()
	{
		view = View::SEARCH;
		channelOpen = false;
		results = searchResults;
		sel = scroll = 0;
		mode = results.empty() ? Mode::KEYS : Mode::RESULTS;
		status = results.empty() ? "" : std::to_string(results.size()) + " results   SEL " + std::to_string(quality) + "p";
		resetThumbs();
	}
	// the Local tab: the music and video folders' own files, one level down. Nothing is fetched and nothing is
	// remembered: a file on the card is played where it lies (mpv takes the path), and only the card's own folders are
	// read, so a missing one is simply empty.
	static bool mediaName(const std::string& n)
	{
		static const char* ext[] = { ".mp3", ".flac", ".wav", ".ogg", ".m4a", ".opus", ".aac",
			".mp4", ".mkv", ".webm", ".mov", ".m4v", ".avi" };
		std::string l = n;
		std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		for (const char* x : ext)
			if (l.size() > strlen(x) && l.compare(l.size() - strlen(x), strlen(x), x) == 0) return true;
		return false;
	}
	void scanLocal(const std::string& dir, std::vector<Item>& out)
	{
		DIR* d = opendir(dir.c_str());
		if (!d) return;
		std::string label = dir.substr(dir.find_last_of('/') + 1);	// "music" / "videos"
		while (dirent* e = readdir(d))
		{
			std::string n = e->d_name;
			if (n.empty() || n[0] == '.' || !mediaName(n)) continue;
			Item it;
			it.id = dir + "/" + n;
			it.title = n;
			it.uploader = label;
			it.local = true;
			out.push_back(it);
		}
		closedir(d);
	}
	void showLocal()
	{
		view = View::LOCAL;
		channelOpen = false;
		localItems.clear();
		scanLocal("/data/music", localItems);
		scanLocal("/data/videos", localItems);
		std::sort(localItems.begin(), localItems.end(), [](const Item& a, const Item& b) { return a.title < b.title; });
		results = localItems;
		sel = scroll = 0;
		mode = Mode::RESULTS;	// an empty list still shows as the tab, with its own hint
		status = localItems.empty() ? "No music or video files in /data/music or /data/videos"
			: std::to_string(localItems.size()) + " files on the card   A plays one";
		resetThumbs();	// no network thumbnails: a file on the card has none
	}
	void cycleTab(int dir)
	{
		int i = view == View::SEARCH ? 0 : view == View::RECENT ? 1 : view == View::CHANNELS ? 2 : 3;
		i = (i + dir + 4) % 4;
		if (i == 0) showSearchTab();
		else if (i == 1) showRecent();
		else if (i == 2) showChannels();
		else showLocal();
	}
	void qualityLoad()
	{
		FILE* f = fopen(CONF_FILE, "r");
		if (!f) return;
		char line[256];
		while (fgets(line, sizeof line, f))
		{
			int q = 0;
			if (sscanf(line, " quality = %d", &q) == 1 || sscanf(line, "quality = %d", &q) == 1)
				if (q == 720 || q == 480 || q == 360) quality = q;
		}
		fclose(f);
	}
	void qualitySave()
	{
		FILE* f = fopen(CONF_FILE, "w");
		if (!f) return;
		fprintf(f, "# banana-tube: the playback quality cap in pixels (Select on the results cycles it)\nquality = %d\n", quality);
		fclose(f);
	}
	// 720 -> 480 -> 360 -> 720: the a613 holds 720 comfortably; the lower caps are for a weak link
	void cycleQuality()
	{
		quality = quality == 720 ? 480 : quality == 480 ? 360 : 720;
		qualitySave();
		if (mode == Mode::RESULTS)
		{
			std::string who = view == View::RECENT ? "Recent"
				: view == View::CHANNELS && !channelOpen ? std::to_string(channels.size()) + " channels"
				: view == View::CHANNELS ? "Channel: " + channelName
				: view == View::LOCAL ? std::to_string(localItems.size()) + " files"
				: std::to_string(results.size()) + " results";
			status = who + "   SEL " + std::to_string(quality) + "p (next video)";
		}
		else status = "Quality: " + std::to_string(quality) + "p (next video)";
	}
	// Y in the player: a resolved stream cannot change quality in place, so stop, re-resolve at the next cap and
	// replay from where it was (play()'s resume path does the seek once mpv is up)
	void reloadQuality()
	{
		if (mode != Mode::PLAYING || playingItem.id.empty()) return;
		Item again = playingItem;
		again.resume = pos;
		stopPlayback();
		quality = nextQuality(quality);
		qualitySave();
		showInd("Reloading at " + std::to_string(quality) + "p\u2026", 1500);
		play(again);
	}
	// ---- mpv -----------------------------------------------------------------------------------------------------
	void ipcSend(const std::string& command)
	{
		if (ipc < 0) return;
		std::string msg = command + "\n";
		if (write(ipc, msg.data(), msg.size()) < 0) {}
	}
	// a transient on-screen indicator, drawn by mpv over the video (its window covers ours): it hides itself after
	// `ms`, so a tap or a d-pad seek shows what it did without a control bar to hide
	void showInd(const std::string& t, int ms = 700)
	{
		if (ipc < 0) return;
		ipcSend("{\"command\":[\"show-text\",\"" + jsonEsc(t) + "\"," + std::to_string(ms) + "]}");
	}
	// the resume seek waits for the first time-pos reply: a seek sent before mpv has opened the stream is dropped
	void applyPendingSeek()
	{
		if (pendingSeek <= 0) return;
		ipcSend("{\"command\":[\"seek\"," + std::to_string(pendingSeek) + ",\"absolute\"]}");
		showInd("Resumed at " + fmtTime(pendingSeek), 1500);
		pendingSeek = 0;
		havePos = false;
	}
	// one non-blocking attempt (the main loop retries every pass, so playback starts without a stall)
	bool ipcTryConnect()
	{
		if (ipc >= 0) return true;
		int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0) return false;
		sockaddr_un sa {};
		sa.sun_family = AF_UNIX;
		snprintf(sa.sun_path, sizeof sa.sun_path, "%s", IPC_SOCK);
		if (connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) { close(fd); return false; }
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);	// pollIpc reads whatever mpv has said
		ipc = fd;
		return true;
	}
	void connectIpc()
	{
		if (ipc >= 0) return;
		for (int i = 0; i < 50; i++)
		{
			if (ipcTryConnect()) return;
			usleep(100000);
		}
	}
	// the stream's direct URLs from yt-dlp: the picture and, for a DASH pair, the sound. The format chain caps it at 720p
	// and skips AV1, which the a613 cannot decode
	bool resolveStream(const std::string& id, std::vector<std::string>& urls)
	{
		// the cap is the player's quality setting (cycleQuality); AV1 is left out, the a613 has no decoder for it
		const std::string h = std::to_string(quality);
		const std::string fmt =
			"bestvideo[height<=" + h + "][vcodec^=avc1]+bestaudio"
			"/bestvideo[height<=" + h + "][vcodec^=vp9]+bestaudio"
			"/bestvideo[height<=" + h + "][vcodec!*=av01]+bestaudio"
			"/best[height<=" + h + "][vcodec!*=av01]/best[height<=" + h + "]/best";
		std::string cmd = "timeout 60 yt-dlp -g --no-warnings --no-playlist -f " + shellQuote(fmt) + " " +
			shellQuote("https://www.youtube.com/watch?v=" + id) + " 2>/dev/null";
		FILE* p = popen(cmd.c_str(), "r");
		if (!p) return false;
		char line[4096];
		while (fgets(line, sizeof line, p))
		{
			std::string s = line;
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
			if (!s.empty()) urls.push_back(s);
		}
		pclose(p);
		return !urls.empty() && urls.size() <= 2;
	}
	// mpv's replies (the time-pos asked for by sbTick), read on every pass so the socket never backs up
	void pollIpc()
	{
		if (ipc < 0) return;
		char buf[1024];
		ssize_t n;
		while ((n = read(ipc, buf, sizeof buf)) > 0) ipcBuf.append(buf, (size_t)n);
		size_t nl;
		while ((nl = ipcBuf.find('\n')) != std::string::npos)
		{
			std::string line = ipcBuf.substr(0, nl);
			ipcBuf.erase(0, nl + 1);
			banana::JPtr r = banana::parseJson(line);
			if (!r || !r->isObj()) continue;
			long id = static_cast<long>(r->num("request_id", -1));
			if (id != POS_REQ && id != DUR_REQ && id != BUF_REQ && id != CACHE_REQ) continue;
			const banana::JVal* d = r->get("data");
			if (id == BUF_REQ)
			{
				bool st = d && (d->kind == banana::JVal::BOOL ? d->b : d->isNum() && d->n != 0);
				if (st && !stallShown) { stallShown = true; showInd("Buffering" + std::string(bufPct < 100 ? " " + std::to_string(bufPct) + "%" : "\u2026"), 1200); }
				else if (!st && stallShown) { stallShown = false; showInd("Playing   " + whereTag(pos), 700); }
				stalled = st;
				continue;
			}
			if (id == CACHE_REQ)
			{
				if (d && d->isNum())
				{
					int p = (int)d->n;
					if (stalled && p / 10 != bufPct / 10) showInd("Buffering " + std::to_string(p) + "%", 1200);
					bufPct = p;
				}
				continue;
			}
			bool ok = d && d->isNum();
			if (id == POS_REQ) { havePos = ok; if (ok) pos = d->n; if (ok && pendingSeek > 0) applyPendingSeek(); }
			else if (ok) dur = d->n;
		}
	}
	// SponsorBlock: ask mpv where playback is, four times a second, and seek past a segment it has reached
	void sbTick(Uint32 now)
	{
		if (ipc < 0) return;
		if (now - lastAsk >= 250)
		{
			lastAsk = now;
			ipcSend("{\"command\":[\"get_property\",\"time-pos\"],\"request_id\":7}");
		}
		if (now - lastDurAsk >= 2000)	// the length changes little: ask it rarely, for Select's readout
		{
			lastDurAsk = now;
			ipcSend("{\"command\":[\"get_property\",\"duration\"],\"request_id\":8}");
		}
		if (now - lastBufAsk >= 500)	// stalled on the network, or filling mpv's cache again
		{
			lastBufAsk = now;
			ipcSend("{\"command\":[\"get_property\",\"paused-for-cache\"],\"request_id\":9}");
			ipcSend("{\"command\":[\"get_property\",\"cache-buffering-state\"],\"request_id\":10}");
		}
		if (now - lastResumeSave >= 10000 && havePos)	// the resume point, every 10 s (flash-friendly)
		{
			lastResumeSave = now;
			saveProgress();
		}
		if (segs.empty() || !havePos) return;
		for (size_t i = 0; i < segs.size(); i++)
		{
			Skip& g = segs[i];
			if (g.done || pos < g.s || pos >= g.e) continue;
			g.done = true;
			lastSkip = static_cast<int>(i);
			ipcSend("{\"command\":[\"seek\"," + std::to_string(g.e) + ",\"absolute\"]}");
			showInd("Skipped " + g.cat + "  (X undo)", 1500);
			havePos = false;
			return;
		}
	}
	// X in the player: back to the start of the last segment skipped, which then plays through
	void sbUndo()
	{
		if (lastSkip < 0 || lastSkip >= static_cast<int>(segs.size())) return;
		const Skip& g = segs[static_cast<size_t>(lastSkip)];
		ipcSend("{\"command\":[\"seek\"," + std::to_string(g.s) + ",\"absolute\"]}");
		showInd("Unskipped " + g.cat, 1200);
		lastSkip = -1;
	}
	// the API's answer ([{"segment":[start,end],"category":"sponsor",...}, ...]); "Not Found" (no segments for the
	// video) and anything else that isn't that array give none
	static std::vector<Skip> sbParse(const std::string& body)
	{
		std::vector<Skip> out;
		banana::JPtr doc = banana::parseJson(body);
		if (!doc || !doc->isArr()) return out;
		for (const banana::JPtr& e : doc->arr)
		{
			const banana::JVal* seg = e ? e->get("segment") : nullptr;
			if (!seg || !seg->isArr() || seg->arr.size() < 2 || !seg->arr[0] || !seg->arr[1]) continue;
			out.push_back(Skip { seg->arr[0]->n, seg->arr[1]->n, e->str("category", "segment"), false });
		}
		return out;
	}
	// the video's skip segments, looked up before it plays (a failure just plays it with no skips). Only the video id
	// leaves the handheld; the lookup is SponsorBlock's public API
	void sbFetch(const std::string& id)
	{
		segs.clear();
		lastSkip = -1;
		std::string url = "https://sponsor.ajay.app/api/skipSegments?videoID=" + id +
			"&categories=%5B%22sponsor%22,%22selfpromo%22,%22interaction%22,%22intro%22,%22outro%22%5D";
		segs = sbParse(curlBody(url, nullptr, nullptr, 4));
	}
	void play(const Item& it)
	{
		markDirty();
		if (mpv > 0) { status = "Already playing"; return; }
		unlink(IPC_SOCK);
		pendingSeek = 0;
		playingId.clear();
		// yt-dlp resolves the stream here, so mpv needs no Lua hook of its own (the image's mpv has none); a file on the
		// card is already its own URL, and has no SponsorBlock to ask about
		std::vector<std::string> urls;
		if (it.local) urls = { it.id };
		else
		{
			if (!resolveStream(it.id, urls)) { status = "Could not load " + it.title; return; }
			sbFetch(it.id);
		}
		std::string url = urls[0];
		const char* argv[] = {
			"mpv",
			"--no-config", "--no-terminal", "--input-default-bindings=no",
			"--input-ipc-server=" IPC_SOCK,
			"--vo=wlshm",							// stable software output (the GPU VO goes white here)
			"--hwdec=auto-copy",					// hardware decode where a copy-back path exists, else software
			"--ao=alsa", "--audio-format=s16",
			"--audio-buffer=0.4",					// small, matched to the AudioReach period
			"--cache=yes",							// stream from a small buffer so seeks/pauses do not stall
			// No mpv OSC: its buttons are tiny and its touch hit-testing drifted on this stack. banana-tube draws
			// nothing over the video (mpv's window covers it), so playback is driven by *large* touch zones on the
			// screen, mapped by banana-tube's own (verified) evdev touch: see tapAt's PLAYING case. osd-playing-msg
			// shows the (gamepad) controls briefly at the start.
			"--osd-level=1", "--osd-font-size=40", "--osd-duration=700",
			// libass here is built without fontconfig ("can't find selected font provider"), so the OSD needs the
			// libass provider and an explicit fonts dir + family. Pixelify Sans is BananaOS's own face; mpv falls
			// back to its default when the family is not installed.
			"--osd-color=#ffefb2", "--osd-border-color=#17140d", "--osd-back-color=#201b11",
			"--osd-font-provider=none", "--osd-fonts-dir=/usr/share/banana/rnds/fonts", "--osd-font=Pixelify Sans",
			"--fullscreen", "--keep-open=no", "--force-window=yes",
			"--msg-level=all=warn",
		};
		// the start-of-playback card: the video, the cap, and the whole pad map (mpv's only OSD here)
		std::string title = it.title.size() > 52 ? it.title.substr(0, 50) + "\u2026" : it.title;
		std::string osdMsg = "--osd-playing-msg=" + title + "   " + std::to_string(quality) +
			"p\nA pause   B stop   D-pad seek   L/R chapter   L2/R2 hold   Y quality   SEL where   START full";
		// a DASH pair: the video is the file, the audio its own stream
		std::string audio = urls.size() > 1 ? "--audio-file=" + urls[1] : std::string();
		std::vector<char*> av;
		for (const char* a : argv) av.push_back(const_cast<char*>(a));
		av.push_back(&osdMsg[0]);
		if (!audio.empty()) av.push_back(&audio[0]);
		av.push_back(&url[0]);
		av.push_back(nullptr);
		logLine("playing " + it.id);
		pid_t pid = fork();
		if (pid == 0)
		{
			setsid();
			execvp("mpv", av.data());
			_exit(127);
		}
		if (pid < 0) { status = "Could not start mpv"; return; }
		mpv = pid;
		mode = Mode::PLAYING;
		paused = false;
		trigL = Trigger();
		trigR = Trigger();
		havePos = false;
		ipcBuf.clear();
		playingId = it.id;
		playingItem = it;
		// a video re-opened from Search resumes where its Recent row stopped, not from the top
		double at = it.resume;
		if (at <= 0)
			for (const Item& o : recent) if (o.id == it.id) { at = o.resume; break; }
		pendingSeek = at > 5 ? at : 0;
		lastResumeSave = SDL_GetTicks();
		status = "Playing: " + it.title + (pendingSeek > 0 ? "   resume " + fmtTime(pendingSeek) : "");
		if (!it.local) recentAdd(it);	// the recent list (SELECT) keeps what actually played (a local file has no video id)
	}
	// graceful stop: ask mpv to quit (so it releases ALSA cleanly), then TERM, then KILL. A hard kill mid-stream
	// leaves the AudioReach frontend to time out, which is the EBUSY the next open would hit.
	void stopPlayback()
	{
		markDirty();
		saveProgress();	// where the viewer stopped, for the Recent tab
		if (mpv > 0)
		{
			ipcSend("{\"command\":[\"quit\"]}");
			for (int i = 0; i < 12 && mpv > 0; i++) { int st = 0; if (waitpid(mpv, &st, WNOHANG) == mpv) { mpv = -1; break; } usleep(50000); }
			if (mpv > 0) { kill(-mpv, SIGTERM); kill(mpv, SIGTERM); }
			for (int i = 0; i < 20 && mpv > 0; i++) { int st = 0; if (waitpid(mpv, &st, WNOHANG) == mpv) { mpv = -1; break; } usleep(50000); }
			if (mpv > 0) { kill(-mpv, SIGKILL); kill(mpv, SIGKILL); waitpid(mpv, nullptr, 0); mpv = -1; }
		}
		if (ipc >= 0) { close(ipc); ipc = -1; }
		unlink(IPC_SOCK);
		playingId.clear();
	}
	bool mpvRunning()
	{
		if (mpv <= 0) return false;
		int st = 0;
		pid_t r = waitpid(mpv, &st, WNOHANG);
		if (r == mpv || (r < 0 && errno == ECHILD)) { mpv = -1; if (ipc >= 0) { close(ipc); ipc = -1; } unlink(IPC_SOCK); return false; }
		return true;
	}
	void logLine(const std::string& s)
	{
		fprintf(stderr, "banana-tube: %s\n", s.c_str());
		fflush(stderr);
	}

	// ---- input actions -------------------------------------------------------------------------------------------
	void back()
	{
		if (mode == Mode::PLAYING) { stopPlayback(); mode = Mode::RESULTS; }
		else if (mode == Mode::RESULTS)
		{
			if (view == View::CHANNELS && channelOpen) showChannels();	// out of a channel, back to the channel list
			else { view = View::SEARCH; channelOpen = false; mode = Mode::KEYS; }
		}
		else if (!query.empty()) { query.clear(); }
		else quit = true;				// leave the loop; the loop quits SDL after it (never SDL_Quit() inside it)
	}
	void activate()
	{
		if (mode == Mode::KEYS)
		{
			if (keyRow >= ROWS)	// the action row
			{
				static const char* acts[] = { "Space", "Del", "Shift", "Search" };
				std::string a = acts[keyCol < 4 ? keyCol : 3];
				if (a == "Space") query += " ";
				else if (a == "Del") { if (!query.empty()) query.pop_back(); }
				else if (a == "Shift") upper = !upper;
				else startSearch();
			}
			else
			{
				const char* k = (upper ? UPPER : LOWER)[keyRow][keyCol];
				if (*k) query += k;
			}
		}
		else if (mode == Mode::RESULTS)
		{
			if (view == View::CHANNELS && !channelOpen)
			{
				if (sel >= 0 && sel < static_cast<int>(channels.size())) openChannel(channels[sel]);
			}
			else if (sel >= 0 && sel < static_cast<int>(results.size())) play(results[sel]);
		}
		else if (mode == Mode::PLAYING)
		{
			connectIpc();
			ipcSend("{\"command\":[\"cycle\",\"pause\"]}");
			paused = !paused;
			showInd((paused ? "||  Paused" : ">  Playing") + std::string("   ") + whereTag(pos), 900);
		}
	}
	void move(int dx, int dy)
	{
		markDirty();
		if (mode == Mode::KEYS)
		{
			if (dx != 0)
			{
				if (keyRow >= ROWS) keyCol = (keyCol + dx + 4) % 4;
				else keyCol = (keyCol + dx + COLS) % COLS;
			}
			if (dy != 0)
			{
				// up/down keep the column. The action row's four buttons sit under the key columns they are drawn
				// beneath, so crossing into or out of it converts the column the same way.
				int nr = keyRow + dy;
				if (nr < 0) nr = ROWS;
				if (nr > ROWS) nr = 0;
				bool wasAct = keyRow >= ROWS, isAct = nr >= ROWS;
				if (wasAct && !isAct) keyCol = actToKey(keyCol);
				else if (!wasAct && isAct) keyCol = keyToAct(keyCol);
				keyRow = nr;
			}
		}
		else if (mode == Mode::RESULTS)
		{
			int n = listSize();
			if (n == 0) return;
			sel += dy != 0 ? dy : dx;
			if (sel < 0) sel = 0;
			if (sel >= n) sel = n - 1;
		}
		else if (mode == Mode::PLAYING)
		{
			connectIpc();
			if (dy != 0) { ipcSend(dy < 0 ? "{\"command\":[\"seek\",\"10\"]}" : "{\"command\":[\"seek\",\"-10\"]}"); seekInd(dy < 0 ? ">> +10" : "<< -10", dy < 0 ? 10 : -10); }
			if (dx != 0) { ipcSend(dx < 0 ? "{\"command\":[\"seek\",\"-60\"]}" : "{\"command\":[\"seek\",\"60\"]}"); seekInd(dx < 0 ? "<< -60" : ">> +60", dx < 0 ? -60 : 60); }
		}
	}
	// The L2/R2 triggers (read as axes; SDL sends no button for them): a tap seeks 10 s, holding R2 plays at 2x, holding
	// L2 scrubs back 10 s every quarter second. A press shorter than the hold is the tap; the hold starts after 300 ms.
	struct Trigger { bool down = false, held = false, fast = false; Uint32 at = 0, lastStep = 0; };
	Trigger trigL, trigR;
	void trigger(Trigger& t, bool isDown, int dir, Uint32 now)
	{
		if (isDown && !t.down)
		{
			connectIpc();
			t = Trigger();
			t.down = true;
			t.at = now;
			return;
		}
		if (!isDown && t.down)
		{
			t.down = false;
			if (t.fast) { ipcSend("{\"command\":[\"set_property\",\"speed\",1]}"); showInd(">  1x", 700); }
			else if (!t.held) { ipcSend(dir > 0 ? "{\"command\":[\"seek\",\"10\"]}" : "{\"command\":[\"seek\",\"-10\"]}"); seekInd(dir > 0 ? ">> +10" : "<< -10", dir * 10.0); }
			t.held = t.fast = false;
			return;
		}
		if (!t.down || mode != Mode::PLAYING || now - t.at < 300) return;
		if (!t.held)
		{
			t.held = true;
			t.lastStep = 0;
			if (dir > 0) { t.fast = true; ipcSend("{\"command\":[\"set_property\",\"speed\",2]}"); showInd(">>  2x", 900); }
		}
		if (!t.fast && now - t.lastStep >= 250)
		{
			t.lastStep = now;
			ipcSend("{\"command\":[\"seek\",\"-10\"]}");
		}
	}
	void page(int dir)
	{
		if (mode != Mode::RESULTS) return;
		int n = listSize(), step = resultsPerPage();
		if (n == 0) return;
		sel += dir * step;
		if (sel < 0) sel = 0;
		if (sel >= n) sel = n - 1;
	}

	// ---- shared layout geometry (draw and touch use the SAME rects, so a tap can never land beside what is drawn) --
	// the top bar: Back, the search field, and the tab strip under them
	int brandH(int H) const { return H * 7 / 100; }
	int contentTop(int H) const { return H * 285 / 1000; }
	int contentBottom(int H) const { return H * 88 / 100; }
	int hintTop(int H) const { return H * 895 / 1000; }
	int hintH(int H) const { return H * 7 / 100; }
	SDL_Rect frameRect(int W, int H) const { return SDL_Rect { W * 2 / 100, H * 8 / 100, W * 96 / 100, H * 89 / 100 }; }
	SDL_Rect hintRect(int W, int H) const { return SDL_Rect { W * 4 / 100, hintTop(H), W * 92 / 100, hintH(H) }; }
	SDL_Rect backRect(int W, int H) const { return SDL_Rect { W * 4 / 100, H * 95 / 1000, W * 10 / 100, H * 7 / 100 }; }
	SDL_Rect queryRect(int W, int H) const { return SDL_Rect { W * 15 / 100, H * 95 / 1000, W * 81 / 100, H * 7 / 100 }; }
	SDL_Rect tabRect(int W, int H, int i) const
	{
		int gap = W * 15 / 1000, tw = (W * 92 / 100 - 3 * gap) / 4;
		return SDL_Rect { W * 4 / 100 + i * (tw + gap), H * 175 / 1000, tw, H * 65 / 1000 };
	}

	SDL_Rect keyRect(int W, int H, int r, int c) const
	{
		int gap = W * 1 / 100;
		int kw = (W * 92 / 100 - (COLS - 1) * gap) / COLS;
		int kh = (contentBottom(H) - contentTop(H) - 4 * gap) / 5;
		int x0 = W * 4 / 100;
		return SDL_Rect { x0 + c * (kw + gap), contentTop(H) + r * (kh + gap), kw, kh };
	}
	SDL_Rect actRect(int W, int H, int c) const
	{
		int gap = W * 1 / 100;
		int kh = (contentBottom(H) - contentTop(H) - 4 * gap) / 5;
		int aw = (W * 92 / 100 - 3 * gap) / 4;
		int x0 = W * 4 / 100;
		return SDL_Rect { x0 + c * (aw + gap), contentTop(H) + ROWS * (kh + gap), aw, kh };
	}
	int resultsTop(int H) const { return contentTop(H); }
	int resultsRowH(int H) const { return std::max(56, H * 14 / 100); }
	int rowsPerPage(int H) const
	{
		int per = (contentBottom(H) - contentTop(H)) / resultsRowH(H);
		return per < 1 ? 1 : per;
	}
	static bool inRect(const SDL_Rect& r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

	// a tap (touch or pointer) at output-space (x,y): the top bar's Back / query, then the current screen's targets
	void tapAt(int x, int y)
	{
		if (getenv("BANANA_TUBE_TOUCH_DEBUG")) { char b[64]; snprintf(b, sizeof b, "tap %d %d (mode %d)", x, y, (int)mode); logLine(b); }
		int W = 0, H = 0;
		SDL_GetRendererOutputSize(ren, &W, &H);
		SDL_Rect backBtn = backRect(W, H);
		SDL_Rect queryBox = queryRect(W, H);
		if (inRect(backBtn, x, y)) { logLine("tap -> Back"); back(); return; }
		if (inRect(queryBox, x, y))
		{
			logLine("tap -> query box");
			if (mode != Mode::PLAYING) { view = View::SEARCH; channelOpen = false; mode = Mode::KEYS; }
			return;
		}
		if (mode != Mode::PLAYING)
			for (int i = 0; i < 4; i++)
				if (inRect(tabRect(W, H, i), x, y))
				{
					static const char* names[] = { "search", "recent", "channels", "local" };
					logLine(std::string("tap -> tab ") + names[i]);
					if (i == 0) showSearchTab(); else if (i == 1) showRecent(); else if (i == 2) showChannels(); else showLocal();
					return;
				}
		if (mode == Mode::KEYS)
		{
			const char* (*keys)[11] = upper ? UPPER : LOWER;
			for (int r = 0; r < ROWS; r++)
				for (int c = 0; c < COLS; c++)
					if (inRect(keyRect(W, H, r, c), x, y))
					{
						char b[64];
						snprintf(b, sizeof b, "tap -> key r%d c%d '%s'", r, c, keys[r][c]);
						logLine(b);
						keyRow = r; keyCol = c; activate(); return;
					}
			static const char* acts[] = { "Space", "Del", "Shift", "Search" };
			for (int c = 0; c < 4; c++)
				if (inRect(actRect(W, H, c), x, y)) { logLine(std::string("tap -> action ") + acts[c]); keyRow = ROWS; keyCol = c; activate(); return; }
		}
		else if (mode == Mode::RESULTS)
		{
			int top = resultsTop(H), rh = resultsRowH(H);
			if (y >= top)
			{
				int idx = scroll + (y - top) / rh;
				if (view == View::CHANNELS && !channelOpen)
				{
					if (idx >= 0 && idx < static_cast<int>(channels.size()))
					{
						if (idx == sel) openChannel(channels[idx]);
						else sel = idx;
					}
					return;
				}
				if (idx >= 0 && idx < (int)results.size())
				{
					char b[64];
					snprintf(b, sizeof b, "tap -> result %d (sel %d)", idx, sel);
					logLine(b);
					if (idx == sel) play(results[idx]);	// a tap on the focused row plays it
					else sel = idx;						// a first tap just focuses
					return;
				}
			}
		}
		else if (mode == Mode::PLAYING)
		{
			// The player controls: no mpv OSC (its buttons are tiny), so the whole screen is the control surface,
			// mapped by banana-tube's own (verified) evdev touch. Four large zones:
			//   top-left        stop and go back
			//   left 28%        seek -10 s
			//   right 28%       seek +10 s
			//   the rest        play / pause
			connectIpc();
			if (x < W * 18 / 100 && y < H * 18 / 100) { logLine("player -> stop"); stopPlayback(); mode = Mode::RESULTS; status = "Stopped"; }
			else if (x < W * 28 / 100) { logLine("player -> seek -10"); ipcSend("{\"command\":[\"seek\",\"-10\"]}"); seekInd("<< -10", -10); }
			else if (x > W * 72 / 100) { logLine("player -> seek +10"); ipcSend("{\"command\":[\"seek\",\"10\"]}"); seekInd(">> +10", 10); }
			else { logLine("player -> play/pause"); ipcSend("{\"command\":[\"cycle\",\"pause\"]}"); paused = !paused;
				showInd((paused ? "||  Paused" : ">  Playing") + std::string("   ") + whereTag(pos), 900); }
		}
	}

	// ---- evdev touch ---------------------------------------------------------------------------------------------
	// SDL2's Wayland backend does not bind wl_touch, so under cage the app receives no SDL_FINGER and no emulated
	// pointer. The panel's touchscreen (FT5x06) is read from evdev here, with the same mapping banana-ui uses: scale
	// the raw axes to the panel's native mode, then turn them by the panel's rotation into the app surface's own
	// (upright) 1240x1080 space. The turn defaults to 90 (the RPC's panel) and can be overridden on the card with
	// /data/banana/bananatube/touch-rot or BANANA_TUBE_TOUCH_ROT, so a card that is mounted the other way is fixed
	// without a rebuild.
	int touchFd = -1;
	int tMinX = 0, tMaxX = 0, tMinY = 0, tMaxY = 0;
	int tX = 0, tY = 0;
	bool tDown = false, tWasDown = false;
	bool tXse = false, tYse = false, tLatched = false;	// both axes seen for this contact; its press point latched
	int tDownX = 0, tDownY = 0;							// the latched press point (raw)
	Uint32 tLastFire = 0;								// one tap per gesture; swallows a finger's chatter
	int panelW = 1080, panelH = 1240, panelRot = 90;
	bool flipX = false, flipY = false;	// extra calibration, from /data/banana/bananatube/touch.conf

	void openTouch()
	{
		// the panel's turn and any flips: /data/banana/bananatube/touch.conf (rot=90, flipx=0, flipy=0), then the
		// env override. A card whose panel is mounted the other way is corrected by editing the file and relaunching.
		if (FILE* f = fopen("/data/banana/bananatube/touch.conf", "r"))
		{
			char line[128];
			while (fgets(line, sizeof line, f))
			{
				int v = 0;
				if (sscanf(line, "rot=%d", &v) == 1) panelRot = v;
				else if (sscanf(line, "flipx=%d", &v) == 1) flipX = v != 0;
				else if (sscanf(line, "flipy=%d", &v) == 1) flipY = v != 0;
			}
			fclose(f);
		}
		if (const char* r = getenv("BANANA_TUBE_TOUCH_ROT")) panelRot = atoi(r);
		// the panel's native mode (the touchscreen's own axes): /sys/class/drm/card*-DSI-1/modes -> "1080x1240"
		if (FILE* f = fopen("/sys/class/drm/card1-DSI-1/modes", "r"))
		{
			int w = 0, h = 0;
			if (fscanf(f, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) { panelW = w; panelH = h; }
			fclose(f);
		}
		DIR* d = opendir("/dev/input");
		if (!d) return;
		while (dirent* e = readdir(d))
		{
			if (strncmp(e->d_name, "event", 5)) continue;
			std::string path = std::string("/dev/input/") + e->d_name;
			int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
			if (fd < 0) continue;
			unsigned long abs[ABS_MAX / (8 * sizeof(long)) + 1] = {};
			ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
			auto bit = [&](int n) { return (abs[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1; };
			// multitouch (ABS_MT_POSITION_X/Y), else a single-touch screen or absolute pointer (ABS_X/Y): the RPC's
			// FT5x06 is multitouch, but a card with a plain touchscreen must work too
			bool mt = bit(ABS_MT_POSITION_X) && bit(ABS_MT_POSITION_Y);
			bool st = !mt && bit(ABS_X) && bit(ABS_Y);
			if (mt || st)
			{
				input_absinfo ax = {}, ay = {};
				ioctl(fd, EVIOCGABS(mt ? ABS_MT_POSITION_X : ABS_X), &ax);
				ioctl(fd, EVIOCGABS(mt ? ABS_MT_POSITION_Y : ABS_Y), &ay);
				tMinX = ax.minimum; tMaxX = ax.maximum; tMinY = ay.minimum; tMaxY = ay.maximum;
				if (tMaxX <= tMinX) tMaxX = tMinX + 1;
				if (tMaxY <= tMinY) tMaxY = tMinY + 1;
				tX = (tMinX + tMaxX) / 2; tY = (tMinY + tMaxY) / 2;
				touchFd = fd;
				break;
			}
			::close(fd);
		}
		closedir(d);
	}
	void mapTouch(int rx, int ry, int W, int H, int& x, int& y)
	{
		float sx = tMaxX > tMinX ? (float)panelW / (tMaxX - tMinX + 1) : 1;
		float sy = tMaxY > tMinY ? (float)panelH / (tMaxY - tMinY + 1) : 1;
		float px = (rx - tMinX) * sx, py = (ry - tMinY) * sy;
		int rxi, ryi;
		switch (panelRot)
		{
		case 90:  rxi = (int)py; ryi = (int)(panelW - 1 - px); break;
		case 270: rxi = (int)(panelH - 1 - py); ryi = (int)px; break;
		case 180: rxi = (int)(panelW - 1 - px); ryi = (int)(panelH - 1 - py); break;
		default:  rxi = (int)px; ryi = (int)py; break;
		}
		if (flipX) rxi = W - 1 - rxi;
		if (flipY) ryi = H - 1 - ryi;
		x = rxi; y = ryi;
	}
	// The touch state machine: one gesture, at most one tap. A tap fires on RELEASE, at a press point latched only
	// once BOTH axes have been seen for this contact -- so a down packet carrying only X (or only Y) can never fire
	// with the previous axis's value (the "66d" that jumped a row before settling), and no frame while the finger is
	// held fires again (the "ghh" a finger's roll used to produce). A repeat tap at the same spot sends no new axis
	// and falls back to the last position; a 120 ms cooldown swallows BTN_TOUCH chatter. Returns true (with outX,
	// outY in the UI's own space) when a tap should be handled.
	bool feedTouch(int type, int code, int value, int W, int H, Uint32 now, int& outX, int& outY)
	{
		if (type == EV_ABS && (code == ABS_MT_POSITION_X || code == ABS_X)) { tX = value; tXse = true; return false; }
		if (type == EV_ABS && (code == ABS_MT_POSITION_Y || code == ABS_Y)) { tY = value; tYse = true; return false; }
		if (type == EV_ABS && code == ABS_MT_TRACKING_ID) { if (value < 0) tDown = false; return false; }
		if (type == EV_KEY && code == BTN_TOUCH) { tDown = value != 0; return false; }
		if (type != EV_SYN || code != SYN_REPORT) return false;
		if (tDown)
		{
			if (!tLatched && tXse && tYse) { tDownX = tX; tDownY = tY; tLatched = true; }
			tWasDown = true;
			return false;
		}
		bool fire = false;
		if (tWasDown && now - tLastFire >= 120)	// one tap per gesture (and no chatter)
		{
			int gx = tLatched ? tDownX : tX, gy = tLatched ? tDownY : tY;	// a repeat tap: no new axis, use the last
			tDownX = gx; tDownY = gy;
			mapTouch(gx, gy, W, H, outX, outY);
			tLastFire = now;
			fire = true;
		}
		tWasDown = false; tXse = tYse = false; tLatched = false;
		return fire;
	}
	void pollTouch()
	{
		if (touchFd < 0) return;
		int W = 0, H = 0;
		SDL_GetRendererOutputSize(ren, &W, &H);
		input_event ev[64];
		ssize_t n;
		while ((n = ::read(touchFd, ev, sizeof ev)) > 0)
		{
			Uint32 now = SDL_GetTicks();
			for (int i = 0; i < (int)(n / sizeof(input_event)); i++)
			{
				const input_event& e = ev[i];
				int x = 0, y = 0;
				if (feedTouch(e.type, e.code, e.value, W, H, now, x, y))
				{
					char b[96];
					snprintf(b, sizeof b, "tap raw %d,%d -> %d,%d (surface %dx%d rot %d)", tDownX, tDownY, x, y, W, H, panelRot);
					logLine(b);
					// the PLAYING case drives mpv over IPC (there is no OSC); elsewhere it is the app's own UI
					markDirty();
					tapAt(x, y);
				}
			}
		}
	}

	// ---- the loop ------------------------------------------------------------------------------------------------
	int run()
	{
		// mpv's window has the focus while a video plays; without this SDL2 drops every pad event the app gets in
		// the background, so the player's pad controls did nothing
		SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
		SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
		TTF_Init();
		IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
		SDL_GameControllerEventState(SDL_ENABLE);
		for (int i = 0; i < SDL_NumJoysticks(); i++)
			if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); if (pad) break; }
		logLine("pads: " + std::to_string(SDL_NumJoysticks()) + (pad ? std::string(" opened: ") + SDL_GameControllerName(pad) : std::string(" none opened")));

		win = SDL_CreateWindow("BananaTube", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 768,
			SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
		if (!win) { logLine(std::string("no window: ") + SDL_GetError()); return 1; }
		ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
		if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
		if (!ren) { logLine(std::string("no renderer: ") + SDL_GetError()); return 1; }
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

		fBig = openFont(fontPathMed, 40, FALLBACK_FONT_MED);
		fMed = openFont(fontPathMed, 30, FALLBACK_FONT_MED);
		fSmall = openFont(fontPath, 22, FALLBACK_FONT);
		if (!fBig || !fMed || !fSmall) { logLine(std::string("no font ") + fontPath + ": " + TTF_GetError()); return 1; }
		{
			int W = 0, H = 0;
			SDL_GetRendererOutputSize(ren, &W, &H);
			if (H > 0)
			{
				TTF_CloseFont(fBig); TTF_CloseFont(fMed); TTF_CloseFont(fSmall);
				fBig = openFont(fontPathMed, std::max(16, H * 5 / 100), FALLBACK_FONT_MED);
				fMed = openFont(fontPathMed, std::max(13, H * 4 / 100), FALLBACK_FONT_MED);
				fSmall = openFont(fontPath, std::max(10, H * 3 / 100), FALLBACK_FONT);
				if (!fBig || !fMed || !fSmall) { logLine("no font at the scaled size"); return 1; }
			}
		}

		if (std::getenv("BANANA_TUBE_SELFTEST")) return selftest();

		loadLogo();
		signal(SIGTERM, onStopSignal);
		signal(SIGINT, onStopSignal);
		signal(SIGHUP, onStopSignal);
		signal(SIGPIPE, SIG_IGN);	// mpv exits at the end of a video: a write to its closed IPC socket must not kill us

		thumbThread = std::thread([this] { thumbWorkerLoop(); });
		openTouch();
		{
			int W = 0, H = 0;
			SDL_GetRendererOutputSize(ren, &W, &H);
			char b[128];
			snprintf(b, sizeof b, "touch: surface %dx%d, panel %dx%d, rot %d, flip %d/%d, fd %d", W, H, panelW, panelH, panelRot, flipX ? 1 : 0, flipY ? 1 : 0, touchFd);
			logLine(b);
		}

		uint32_t lastRepeat = 0;
		int heldX = 0, heldY = 0;

		for (bool alive = true; alive && !g_stop && !quit;)
		{
			SDL_Event ev;
			while (SDL_PollEvent(&ev))
			{
				markDirty();	// a key, the pad, a window resize: whatever it was, the frame may differ
				if (ev.type == SDL_QUIT) alive = false;
				else if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad)
				{
					if (SDL_IsGameController(ev.cdevice.which)) pad = SDL_GameControllerOpen(ev.cdevice.which);
				}
				else if (ev.type == SDL_CONTROLLERBUTTONDOWN)
				{
					switch (ev.cbutton.button)
					{
					case SDL_CONTROLLER_BUTTON_A: activate(); break;
					case SDL_CONTROLLER_BUTTON_B: back(); break;
					case SDL_CONTROLLER_BUTTON_X:	// the player's SponsorBlock undo; a backspace on the keys; a channel on the results
						if (mode == Mode::PLAYING) { connectIpc(); sbUndo(); }
						else if (mode == Mode::KEYS) { if (!query.empty()) query.pop_back(); }
						else if (!results.empty() && sel < static_cast<int>(results.size()))
						{
							if (!results[sel].uploader.empty()) openChannel(results[sel].uploader);
						}
						else page(-1);
						break;
					case SDL_CONTROLLER_BUTTON_Y: if (mode == Mode::RESULTS) { view = View::SEARCH; channelOpen = false; results = searchResults; mode = Mode::KEYS; status = ""; } else if (mode == Mode::KEYS) startSearch(); else if (mode == Mode::PLAYING) reloadQuality(); break;
					case SDL_CONTROLLER_BUTTON_START:
						if (mode == Mode::PLAYING) { connectIpc(); ipcSend("{\"command\":[\"cycle\",\"fullscreen\"]}"); }
						else startSearch();
						break;
					case SDL_CONTROLLER_BUTTON_BACK:	// Select: where playback is, or the quality cap
						if (mode == Mode::PLAYING)
						{
							connectIpc();
							if (havePos && dur > 0) showInd(whereTag(pos) + "\n" + std::to_string(quality) + "p", 2200);
							else showInd("Loading\u2026", 900);
						}
						else cycleQuality();
						break;
					case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
						if (mode == Mode::PLAYING) { connectIpc(); ipcSend("{\"command\":[\"add\",\"chapter\",-1]}"); }
						else cycleTab(-1);
						break;
					case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
						if (mode == Mode::PLAYING) { connectIpc(); ipcSend("{\"command\":[\"add\",\"chapter\",1]}"); }
						else cycleTab(1);
						break;
					case SDL_CONTROLLER_BUTTON_DPAD_UP: move(0, -1); break;
					case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move(0, 1); break;
					case SDL_CONTROLLER_BUTTON_DPAD_LEFT: move(-1, 0); break;
					case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: move(1, 0); break;
					default: break;
					}
				}
				else if (ev.type == SDL_KEYDOWN)
				{
					SDL_Keycode k = ev.key.keysym.sym;
					if (k == SDLK_ESCAPE) alive = false;
					else if (k == SDLK_RETURN) activate();
					else if (k == SDLK_BACKSPACE) { if (!query.empty()) query.pop_back(); }
					else if (k == SDLK_UP) move(0, -1);
					else if (k == SDLK_DOWN) move(0, 1);
					else if (k == SDLK_LEFT) move(-1, 0);
					else if (k == SDLK_RIGHT) move(1, 0);
					else if (k == SDLK_TAB && mode != Mode::PLAYING)	// a desktop keyboard (the QEMU test); the pad uses L/R
						cycleTab((ev.key.keysym.mod & KMOD_SHIFT) ? -1 : 1);
					else if (k >= 32 && k < 127 && mode == Mode::KEYS) query += static_cast<char>(k);
				}
				// Touch is read from evdev (pollTouch), never through SDL: under cage SDL delivers a touch as a
				// *pointer* at offset coordinates, and acting on that too fired a second, wrong tap per gesture (the
				// phantom 'h'/'7' characters). SDL's keyboard events above stay (a desktop keyboard for testing).
			}

			Uint32 now = SDL_GetTicks();
			if (pad)
			{
				int dx = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) - SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
				int dy = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) - SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP);
				if (dx != heldX || dy != heldY) { heldX = dx; heldY = dy; lastRepeat = now; }
				else if ((dx || dy) && now - lastRepeat > 250) { move(dx, dy); lastRepeat = now + 60; }
			}

			if (pad && mode == Mode::PLAYING)
			{
				trigger(trigL, SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000, -1, now);
				trigger(trigR, SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000, 1, now);
			}
			else if (pad && mode == Mode::RESULTS)
			{
				// a page of results on the triggers: the shoulders are the tabs now
				static bool l0 = false, r0 = false;
				bool l = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
				bool r = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
				if (l && !l0) page(-1);
				if (r && !r0) page(1);
				l0 = l;
				r0 = r;
			}

			if (mode == Mode::PLAYING && ipc < 0) ipcTryConnect();
			pollIpc();
			if (mode == Mode::PLAYING) sbTick(now);

			pollSearch();
			pollTouch();
			if (mode == Mode::PLAYING && !mpvRunning())
			{
				saveProgress(true);	// a finished video starts from the top next time
				playingId.clear();
				mode = Mode::RESULTS;
				status = "Playback ended (B to go back)";
				markDirty();
			}

			{
				static unsigned seenGen = 0;	// a thumbnail arriving is the one change with no input behind it
				unsigned g = thumbGeneration.load();
				if (g != seenGen) { seenGen = g; markDirty(); }
			}
			// mpv owns the screen while a video plays: nothing app-side is visible, so draw nothing
			if (dirty && mode != Mode::PLAYING) { draw(); SDL_RenderPresent(ren); dirty = false; }
			SDL_Delay(8);
		}
		stopPlayback();
		if (searchThread.joinable()) searchThread.join();
		{ std::lock_guard<std::mutex> l(thumbMx); thumbStop = true; }
		thumbCv.notify_all();
		if (thumbThread.joinable()) thumbThread.join();
		if (touchFd >= 0) { close(touchFd); touchFd = -1; }
		clearTextCache();
		for (auto& kv : thumbTex) if (kv.second) SDL_DestroyTexture(kv.second);
		if (backdrop) { SDL_DestroyTexture(backdrop); backdrop = nullptr; }
		if (logo) { SDL_DestroyTexture(logo); logo = nullptr; }
		return 0;
	}

	// a headless pass over the pure logic (the harness has no display): the InnerTube walk, the yt-dlp line split,
	// the keyboard navigation and the back()/activate() state machine.
	int selftest()
	{
		{
			// the text cache: the same string, font and colour is one texture; a different colour is another
			clearTextCache();
			size_t m0 = textMisses, h0 = textHits;
			SDL_Color white { 255, 255, 255, 255 }, red { 255, 0, 0, 255 };
			drawText("Cache me", fSmall, white, 0, 0);
			drawText("Cache me", fSmall, white, 0, 0);
			bool ok = textMisses == m0 + 1 && textHits == h0 + 1;
			drawText("Cache me", fSmall, red, 0, 0);
			ok = ok && textMisses == m0 + 2;
			printf("bananatube selftest cache: %s\n", ok ? "ok" : "fail");
			clearTextCache();
		}
		{
			// the Pocket shell: the frame, the header and the tab strip fit at both sizes
			bool ok = true;
			for (const auto& size : { std::pair<int, int> { 1240, 1080 }, std::pair<int, int> { 1024, 768 } })
			{
				int W = size.first, H = size.second;
				SDL_Rect fr = frameRect(W, H), back = backRect(W, H), q = queryRect(W, H), hint = hintRect(W, H);
				SDL_Rect t0 = tabRect(W, H, 0), t3 = tabRect(W, H, 3);
				ok = ok && fr.x >= 0 && fr.y >= 0 && fr.x + fr.w <= W && fr.y + fr.h <= H;
				ok = ok && back.x >= fr.x && q.x + q.w <= fr.x + fr.w;
				ok = ok && back.y + back.h <= t0.y && q.y + q.h <= t0.y;
				for (int i = 0; i < 3; i++)
				{
					SDL_Rect a = tabRect(W, H, i), b = tabRect(W, H, i + 1);
					ok = ok && a.x + a.w <= b.x;
				}
				ok = ok && t0.x >= fr.x && t3.x + t3.w <= fr.x + fr.w && t0.y + t0.h <= contentTop(H);
				ok = ok && contentBottom(H) <= hintTop(H) && hint.y >= contentBottom(H);
				ok = ok && hint.y + hint.h <= fr.y + fr.h;
				ok = ok && resultsTop(H) == contentTop(H) && resultsRowH(H) == std::max(56, H * 14 / 100);
				ok = ok && rowsPerPage(H) >= 4;
				SDL_Rect k0 = keyRect(W, H, 0, 0), kEnd = keyRect(W, H, 3, COLS - 1), aEnd = actRect(W, H, 3);
				ok = ok && k0.x >= fr.x && kEnd.x + kEnd.w <= fr.x + fr.w;
				ok = ok && k0.y == contentTop(H) && aEnd.x + aEnd.w <= fr.x + fr.w && aEnd.y + aEnd.h <= hintTop(H);
			}
			printf("bananatube selftest layout: %s\n", ok ? "ok" : "fail");
		}
		{
			// the position bar and the quality cycle (the player's OSD text)
			std::string a = barText(0, 100, 10), m = barText(50, 100, 10), z = barText(100, 100, 10);
			bool ok = a.size() == 10 && a[0] == '|' && m.size() == 10 && m[5] == '|' && z.size() == 10 && z[9] == '|'
				&& barText(10, 0, 8).size() == 8 && barText(200, 100, 10)[9] == '|';
			ok = ok && nextQuality(720) == 480 && nextQuality(480) == 360 && nextQuality(360) == 720;
			printf("bananatube selftest bar: %s\n", ok ? "ok" : "fail");
		}
		{
			// the thumbnail map follows the visible window: a long browse must not keep every row's texture
			std::vector<Item> back = results;
			results.clear();
			for (const char* id : { "a", "b", "c", "d", "e", "f" })
			{
				Item x;
				x.id = id;
				results.push_back(x);
			}
			bool made = true;
			for (const Item& x : results)
			{
				SDL_Texture* t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STATIC, 2, 2);
				if (t) thumbTex[x.id] = t;
				else made = false;
			}
			thumbWinFirst = thumbWinLast = -1;
			pruneThumbs(1, 4);
			bool ok = made && thumbTex.size() == 3 && thumbTex.count("b") && thumbTex.count("c") && thumbTex.count("d");
			for (auto& kv : thumbTex) if (kv.second) SDL_DestroyTexture(kv.second);
			thumbTex.clear();
			thumbWinFirst = thumbWinLast = -1;
			results = back;
			printf("bananatube selftest thumbs: %s\n", ok ? "ok" : "fail");
		}
		Item it;
		bool ok = splitLine("abc123\tA Title\tSome One\t3:21", it) && it.id == "abc123" && it.duration == "3:21";
		printf("bananatube selftest split: %s\n", ok ? "ok" : "fail");
		{
			// the recent file's optional 5th field: the resume position (older 4-field rows start at 0)
			Item r, q;
			bool r1 = parseRecentLine("abc\tA Title\tSome One\t3:21\t42.5", r) &&
				r.resume == 42.5 && r.id == "abc" && r.duration == "3:21";
			bool r2 = parseRecentLine("abc\tA Title\tSome One\t3:21", q) && q.resume == 0 && q.uploader == "Some One";
			printf("bananatube selftest recent: %s\n", r1 && r2 ? "ok" : "fail");
		}
		{
			// the Local tab's folder walk: media extensions only, file names as titles, one level
			char tmpl[] = "/tmp/bt-local-XXXXXX";
			char* d = mkdtemp(tmpl);
			bool ok = false;
			if (d)
			{
				const char* files[] = { "a.mp3", "b.MKV", "notes.txt" };
				for (const char* f : files)
					if (FILE* fp = fopen((std::string(d) + "/" + f).c_str(), "w")) fclose(fp);
				std::vector<Item> got;
				scanLocal(d, got);
				std::sort(got.begin(), got.end(), [](const Item& a, const Item& b) { return a.title < b.title; });
				ok = got.size() == 2 && got[0].local && got[1].local && got[0].title == "a.mp3" && got[1].title == "b.MKV";
				for (const Item& g : got) remove(g.id.c_str());
				rmdir(d);
			}
			printf("bananatube selftest local: %s\n", ok ? "ok" : "fail");
		}
		std::vector<Skip> seg = sbParse("[{\"segment\":[12.5,20.25],\"category\":\"sponsor\",\"UUID\":\"a\"},"
			"{\"segment\":[0,3.5],\"category\":\"intro\"}]");
		bool segOk = seg.size() == 2 && seg[0].s == 12.5 && seg[0].e == 20.25 && seg[0].cat == "sponsor" && seg[1].cat == "intro";
		printf("bananatube selftest sponsorblock: %s\n", segOk ? "ok" : "fail");
		printf("bananatube selftest sponsorblock none: %s\n", sbParse("Not Found").empty() ? "ok" : "fail");
		{
			Mode saved = mode;
			int r = keyRow, c = keyCol;
			mode = Mode::KEYS;
			keyRow = 1; keyCol = 5;
			move(0, 1); bool a = keyRow == 2 && keyCol == 5;
			move(0, 1); move(0, 1); bool b = keyRow == ROWS && keyCol == 2;
			move(0, -1); bool d = keyRow == 3 && keyCol == 6;
			mode = saved; keyRow = r; keyCol = c;
			printf("bananatube selftest keynav: %s\n", a && b && d ? "ok" : "fail");
		}
		{
			// the Channels tab: one channel's videos, de-duplicated across search hits and recent plays, and B
			// walks out to the channel list before the keyboard
			Mode sm = mode; View sv = view;
			std::vector<Item> s0 = searchResults, s1 = results, s2 = recent;
			bool s3 = channelOpen; std::string s4 = channelName;
			mode = Mode::RESULTS;
			searchResults = { Item { "a", "A", "Chan One", "1:00" }, Item { "b", "B", "Chan Two", "2:00" },
				Item { "c", "C", "Chan One", "3:00" } };
			recent = { Item { "a", "A", "Chan One", "1:00" }, Item { "d", "D", "Chan One", "4:00" } };
			openChannel("Chan One");
			bool ok1 = channelOpen && view == View::CHANNELS && results.size() == 3 &&
				results[0].id == "a" && results[2].id == "d";
			back();
			bool ok2 = !channelOpen && view == View::CHANNELS && mode == Mode::RESULTS;
			mode = sm; view = sv; searchResults = s0; results = s1; recent = s2; channelOpen = s3; channelName = s4;
			printf("bananatube selftest channels: %s\n", ok1 && ok2 ? "ok" : "fail");
		}
		const char* sample = "{\"contents\":{\"x\":{\"contents\":[{\"videoRenderer\":{\"videoId\":\"V1\","
			"\"title\":{\"runs\":[{\"text\":\"Hello World\"}]},\"ownerText\":{\"runs\":[{\"text\":\"Chan\"}]},"
			"\"lengthText\":{\"simpleText\":\"4:07\"}}}]}}}";
		banana::JPtr doc = banana::parseJson(sample);
		std::vector<Item> got;
		if (doc) collectRenderers(doc.get(), got);
		printf("bananatube selftest innertube: %s (%s %s %s)\n", got.size() == 1 ? "ok" : "fail",
			got.empty() ? "" : got[0].id.c_str(), got.empty() ? "" : got[0].title.c_str(), got.empty() ? "" : got[0].duration.c_str());
		query = "cats";
		move(0, 1);
		printf("bananatube selftest keyrow: %d\n", keyRow);
		upper = true;
		activate();
		printf("bananatube selftest typed: %s\n", query.c_str());
		results = { it, it };
		mode = Mode::RESULTS;
		move(0, 1);
		printf("bananatube selftest sel: %d\n", sel);
		back();
		printf("bananatube selftest mode: %d\n", static_cast<int>(mode));
		back();
		printf("bananatube selftest query: '%s'\n", query.c_str());
		back();
		printf("bananatube selftest quit: %d\n", quit ? 1 : 0);
		// the touch transform: the four raw corners of the 1080x1240 panel, turned 90, in the app surface's 1240x1080
		// space. The kernel's multitouch axes are the panel's own (EVIOCGABS: 0..1079 x 0..1239); rot 90 is
		// x' = y, y' = 1079 - x.
		tMinX = 0; tMaxX = 1079; tMinY = 0; tMaxY = 1239; panelRot = 90; flipX = flipY = false;
		static const struct { int rx, ry, ex, ey; const char* name; } corners[] = {
			{ 0, 0, 0, 1079, "tl" }, { 1079, 0, 0, 0, "tr" },
			{ 0, 1239, 1239, 1079, "bl" }, { 1079, 1239, 1239, 0, "br" },
			{ 1077, 25, 25, 2, "spot" },
		};
		for (auto& c : corners)
		{
			int mx = 0, my = 0;
			mapTouch(c.rx, c.ry, 1240, 1080, mx, my);
			printf("bananatube selftest touch %-4s raw %4d,%4d -> %4d,%4d (expect %4d,%4d): %s\n",
				c.name, c.rx, c.ry, mx, my, c.ex, c.ey, (mx == c.ex && my == c.ey) ? "ok" : "FAIL");
		}
		// the state machine ("66d"/"ghh"): a tap whose down packet carries only X, a wobble while held, then release
		// -> exactly one tap, at the press point (never the stale-axis cell or the wobble's), and nothing on later
		// SYN_REPORTs while the finger is down
		{
			int fires = 0, fx = 0, fy = 0;
			auto feed = [&](int t, int c, int v, Uint32 now)
			{
				int x = 0, y = 0;
				if (feedTouch(t, c, v, 1240, 1080, now, x, y)) { fires++; fx = x; fy = y; }
			};
			Uint32 now = 1000;
			tLastFire = 0; tDown = tWasDown = tXse = tYse = tLatched = false;
			tX = 300; tY = 700;	// the previous tap's point (a stale Y is what the "66d" bug fired with)
			feed(EV_KEY, BTN_TOUCH, 1, now);
			feed(EV_ABS, ABS_MT_POSITION_X, 300, now); feed(EV_SYN, SYN_REPORT, 0, now);	// only X so far: must not fire
			feed(EV_ABS, ABS_MT_POSITION_Y, 700, now); feed(EV_SYN, SYN_REPORT, 0, now);	// both axes: the point is latched
			feed(EV_ABS, ABS_MT_POSITION_X, 360, now); feed(EV_SYN, SYN_REPORT, 0, now);	// wobble while held: still nothing
			feed(EV_ABS, ABS_MT_TRACKING_ID, -1, now); feed(EV_SYN, SYN_REPORT, 0, now);	// release: exactly one tap
			printf("bananatube selftest tap: fires %d at %d,%d (expect 1 at 700,779): %s\n",
				fires, fx, fy, (fires == 1 && fx == 700 && fy == 779) ? "ok" : "FAIL");
		}
		return 0;
	}

	int resultsPerPage() const
	{
		int W = 0, H = 0;
		SDL_GetRendererOutputSize(ren, &W, &H);
		return rowsPerPage(H);
	}

	// ---- drawing -------------------------------------------------------------------------------------------------
	// The frame and dot grid never change: bake them into one texture so a dirty frame costs a single copy (A1).
	void ensureBackdrop(int W, int H, const Skin& sk)
	{
		if (backdrop && W == backdropW && H == backdropH) return;
		if (backdrop) { SDL_DestroyTexture(backdrop); backdrop = nullptr; }
		SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
		if (!s) { logLine("no backdrop surface"); return; }
		auto fill = [s](int x, int y, int w, int h, SDL_Color c)
		{
			SDL_Rect r { x, y, w, h };
			SDL_FillRect(s, &r, SDL_MapRGBA(s->format, c.r, c.g, c.b, c.a));
		};
		fill(0, 0, W, H, sk.bg);
		fill(0, 0, W, brandH(H), sk.bezel);
		fill(0, brandH(H) - sk.bw, W, sk.bw, sk.ring);
		SDL_Rect fr = frameRect(W, H);
		fill(fr.x, fr.y, fr.w, fr.h, sk.bezel);
		fill(fr.x + sk.bw, fr.y + sk.bw, fr.w - 2 * sk.bw, fr.h - 2 * sk.bw, sk.ring);
		int in = 2 * sk.bw;
		int px = fr.x + in, py = fr.y + in, pw = fr.w - 2 * in, ph = fr.h - 2 * in;
		fill(px, py, pw, ph, sk.panel);
		for (int y = py + 8; y < py + ph - 2; y += 16)
			for (int x = px + 8; x < px + pw - 2; x += 16)
				fill(x, y, 2, 2, sk.dot);
		SDL_Rect hr = hintRect(W, H);
		fill(hr.x, hr.y, hr.w, hr.h, sk.bezel);
		fill(hr.x, hr.y, hr.w, sk.bw, sk.ring);
		backdrop = SDL_CreateTextureFromSurface(ren, s);
		SDL_FreeSurface(s);
		if (!backdrop) { logLine("no backdrop texture"); return; }
		backdropW = W;
		backdropH = H;
	}
	void loadLogo()
	{
		struct stat st;
		const char* p = !stat("/usr/share/banana/banana/banana-48.png", &st)
			? "/usr/share/banana/banana/banana-48.png" : "/usr/share/banana/banana/banana-32.png";
		SDL_Surface* s = IMG_Load(p);
		if (!s) return;
		logo = SDL_CreateTextureFromSurface(ren, s);
		SDL_FreeSurface(s);
	}

	void draw()
	{
		int W = 0, H = 0;
		SDL_GetRendererOutputSize(ren, &W, &H);
		Skin sk = skin(W);
		ensureBackdrop(W, H, sk);
		if (backdrop) SDL_RenderCopy(ren, backdrop, nullptr, nullptr);
		else { SDL_SetRenderDrawColor(ren, sk.bg.r, sk.bg.g, sk.bg.b, 255); SDL_RenderClear(ren); }

		// the brand strip: the OS banana mark, the BananaTube wordmark and the quality cap
		{
			int ls = H * 45 / 1000, lx = W * 25 / 1000;
			if (logo)
			{
				SDL_Rect lr { lx, (brandH(H) - ls) / 2, ls, ls };
				SDL_RenderCopy(ren, logo, nullptr, &lr);
				lx += ls + W * 12 / 1000;
			}
			int bw1 = 0, wh = 0;
			TTF_SizeUTF8(fMed, "Banana", &bw1, &wh);
			drawText("Banana", fMed, sk.gold, lx, (brandH(H) - wh) / 2);
			drawText("Tube", fMed, sk.ink, lx + bw1, (brandH(H) - wh) / 2);
			std::string cap = std::to_string(quality) + "p";
			int qw = 0, qh = 0;
			TTF_SizeUTF8(fSmall, cap.c_str(), &qw, &qh);
			int pad = W * 15 / 1000, ch = H * 35 / 1000;
			int cx = W - W * 25 / 1000 - (qw + 2 * pad);
			box(cx, (brandH(H) - ch) / 2, qw + 2 * pad, ch, sk.card, sk.edge, sk.bw);
			drawText(cap, fSmall, sk.gold, cx + pad, (brandH(H) - qh) / 2);
		}

		// the header: Back (B's target) and the search well; the rects are the same ones tapAt() hit-tests
		SDL_Rect back = backRect(W, H), queryBox = queryRect(W, H);
		box(back.x, back.y, back.w, back.h, sk.card, sk.edge, sk.bw);
		int bw = 0, bh = 0;
		TTF_SizeUTF8(fSmall, "Back", &bw, &bh);
		drawText("Back", fSmall, sk.ink, back.x + (back.w - bw) / 2, back.y + (back.h - bh) / 2);
		bool typing = mode == Mode::KEYS;
		box(queryBox.x, queryBox.y, queryBox.w, queryBox.h, sk.bezel, typing ? sk.gold : sk.ring, sk.bw);
		std::string shown = query.empty() ? "Search YouTube" : query;
		int qh = 0;
		TTF_SizeUTF8(fBig, shown.c_str(), nullptr, &qh);
		drawText(shown, fBig, query.empty() ? sk.soft : sk.ink, queryBox.x + W * 2 / 100,
			queryBox.y + (queryBox.h - qh) / 2, queryBox.w - W * 4 / 100);

		// the four tabs; the active one is gold, counts ride inline when they fit
		if (mode != Mode::PLAYING)
		{
			static const char* tabNames[] = { "Search", "Recent", "Channels", "Local" };
			for (int i = 0; i < 4; i++)
			{
				SDL_Rect tr = tabRect(W, H, i);
				bool on = i == 0 ? view == View::SEARCH : i == 1 ? view == View::RECENT : i == 2 ? view == View::CHANNELS : view == View::LOCAL;
				bool hot = (i == 1 && !recent.empty()) || (i == 2 && !channels.empty()) || (i == 3 && !localItems.empty());
				std::string label = tabNames[i];
				std::string count = i == 1 && !recent.empty() ? std::to_string(recent.size())
					: i == 2 && !channels.empty() ? std::to_string(channels.size())
					: i == 3 && !localItems.empty() ? std::to_string(localItems.size()) : "";
				if (!count.empty())
				{
					int cw = 0, chh = 0;
					TTF_SizeUTF8(fMed, (label + " " + count).c_str(), &cw, &chh);
					if (cw + W * 2 / 100 <= tr.w) label += " " + count;
				}
				box(tr.x, tr.y, tr.w, tr.h, on ? sk.gold : sk.card, on ? sk.goldHi : sk.line, sk.bw);
				int tw = 0, th = 0;
				TTF_SizeUTF8(fMed, label.c_str(), &tw, &th);
				drawText(label, fMed, on ? sk.bg : hot ? sk.ink : sk.soft, tr.x + (tr.w - tw) / 2,
					tr.y + (tr.h - th) / 2);
			}
		}

		// the results' position ("3 / 24") on the right of the status line, so a long list is never a guess
		int counterW = 0;
		int shownItems = listSize();
		if (mode == Mode::RESULTS && shownItems > 0 && sel >= shownItems) sel = shownItems - 1;
		if (mode == Mode::RESULTS && shownItems > 0)
		{
			std::string pos = std::to_string(sel + 1) + " / " + std::to_string(shownItems);
			int ch = 0;
			TTF_SizeUTF8(fSmall, pos.c_str(), &counterW, &ch);
			drawText(pos, fSmall, sk.ink, W * 96 / 100 - counterW, H * 255 / 1000);
			counterW += W * 3 / 100;
		}
		if (!status.empty()) drawText(status, fSmall, sk.soft, W * 4 / 100, H * 255 / 1000, W * 92 / 100 - counterW);

		if (mode == Mode::KEYS) drawKeyboard(W, H, sk);
		else if (mode == Mode::RESULTS && view == View::CHANNELS && !channelOpen) drawChannels(W, H, sk);
		else drawResults(W, H, sk);

		drawHints(W, H, sk);
	}
	// the menu's button pills: a key chip and what it does, at most four per screen
	void drawHints(int W, int H, const Skin& sk)
	{
		struct Hint { const char* key; const char* label; };
		static const Hint keysH[] = { { "A", "Type" }, { "B", "Back" }, { "Y", "Search" }, { "L/R", "Tab" } };
		static const Hint resultsH[] = { { "A", "Play" }, { "B", "Back" }, { "Y", "Search" }, { "L/R", "Tab" } };
		static const Hint channelH[] = { { "A", "Play" }, { "B", "Back" }, { "L/R", "Tab" } };
		bool inChannel = view == View::CHANNELS && channelOpen;
		const Hint* hs = mode == Mode::KEYS ? keysH : inChannel ? channelH : resultsH;
		int n = mode == Mode::KEYS || !inChannel ? 4 : 3;
		int chipH = H * 42 / 1000, padX = W * 15 / 1000;
		int x = W * 4 / 100, y = hintTop(H) + (hintH(H) - chipH) / 2;
		for (int i = 0; i < n; i++)
		{
			int kw = 0, kh = 0;
			TTF_SizeUTF8(fSmall, hs[i].key, &kw, &kh);
			box(x, y, kw + 2 * padX, chipH, sk.panel, sk.edge, 2);
			drawText(hs[i].key, fSmall, sk.ink, x + padX, y + (chipH - kh) / 2);
			x += kw + 2 * padX + W * 1 / 100;
			int lw = 0, lh = 0;
			TTF_SizeUTF8(fSmall, hs[i].label, &lw, &lh);
			drawText(hs[i].label, fSmall, sk.soft, x, y + (chipH - lh) / 2);
			x += lw + W * 3 / 100;
		}
	}

	void drawKeyboard(int W, int H, const Skin& sk)
	{
		const char* (*keys)[11] = upper ? UPPER : LOWER;
		for (int r = 0; r < ROWS; r++)
			for (int c = 0; c < COLS; c++)
			{
				const char* k = keys[r][c];
				SDL_Rect rr = keyRect(W, H, r, c);
				bool on = keyRow == r && keyCol == c;
				box(rr.x, rr.y, rr.w, rr.h, on ? sk.gold : sk.card, on ? sk.goldHi : sk.line, sk.bw);
				if (on)
				{
					int in = sk.bw + 2;
					box(rr.x + in, rr.y + in, rr.w - 2 * in, rr.h - 2 * in,
						SDL_Color { 0, 0, 0, 0 }, sk.goldHi, 2);
				}
				int tw = 0, th = 0;
				TTF_SizeUTF8(fMed, k, &tw, &th);
				drawText(k, fMed, on ? sk.bg : sk.ink, rr.x + (rr.w - tw) / 2, rr.y + (rr.h - th) / 2);
			}
		const char* acts[] = { "Space", "Del", upper ? "ABC" : "abc", "Search" };
		for (int c = 0; c < 4; c++)
		{
			SDL_Rect rr = actRect(W, H, c);
			bool on = keyRow >= ROWS && keyCol == c;
			bool primary = c == 3 && !query.empty();
			box(rr.x, rr.y, rr.w, rr.h, on ? sk.gold : sk.card,
				on ? sk.goldHi : primary ? sk.gold : sk.line, sk.bw);
			if (on)
			{
				int in = sk.bw + 2;
				box(rr.x + in, rr.y + in, rr.w - 2 * in, rr.h - 2 * in,
					SDL_Color { 0, 0, 0, 0 }, sk.goldHi, 2);
			}
			int tw = 0, th = 0;
			TTF_SizeUTF8(fMed, acts[c], &tw, &th);
			drawText(acts[c], fMed, on ? sk.bg : primary ? sk.gold : sk.ink,
				rr.x + (rr.w - tw) / 2, rr.y + (rr.h - th) / 2);
		}
	}

	void drawResults(int W, int H, const Skin& sk)
	{
		int top = resultsTop(H);
		int rowH = resultsRowH(H);
		int per = resultsPerPage();
		int gap = W * 1 / 100;
		int cardH = rowH - gap;
		int inset = std::max(4, W / 250);
		if (sel < scroll) scroll = sel;
		if (sel >= scroll + per) scroll = sel - per + 1;
		if (searching || results.empty())
		{
			const char* none = searching ? "Searching\u2026"
				: view == View::RECENT ? "Nothing played yet. Play a video and it lands here."
				: "No results. B for the keyboard.";
			drawText(none, fMed, sk.soft, W * 6 / 100, top);
			return;
		}
		// the map carries the visible page and its neighbours only
		int wf = std::max(0, scroll - 1), wl = std::min(static_cast<int>(results.size()), scroll + per + 1);
		if (wf != thumbWinFirst || wl != thumbWinLast) pruneThumbs(wf, wl);
		int thumbH = cardH - 2 * inset, thumbW = thumbH * 16 / 9;
		if (thumbW > W * 32 / 100) { thumbW = W * 32 / 100; thumbH = thumbW * 9 / 16; }
		for (int i = scroll; i < static_cast<int>(results.size()) && i < scroll + per; i++)
		{
			const Item& it = results[i];
			int y = top + (i - scroll) * rowH;
			bool on = i == sel;
			box(W * 4 / 100, y, W * 92 / 100, cardH, sk.card, on ? sk.gold : sk.line, sk.bw);
			if (on)
			{
				SDL_Rect bar { W * 4 / 100, y, std::max(6, W * 8 / 1000), cardH };
				SDL_SetRenderDrawColor(ren, sk.gold.r, sk.gold.g, sk.gold.b, 255);
				SDL_RenderFillRect(ren, &bar);
			}
			int tx = W * 6 / 100;
			SDL_Rect tr { tx, y + inset, thumbW, thumbH };
			SDL_Texture* t = thumb(it.id);
			if (t)
			{
				SDL_RenderCopy(ren, t, nullptr, &tr);
				box(tr.x, tr.y, tr.w, tr.h, SDL_Color { 0, 0, 0, 0 }, sk.line, 2);
			}
			else box(tr.x, tr.y, tr.w, tr.h, sk.ring, sk.line, sk.bw);
			// the duration, on the picture's bottom-right corner (where a video's length belongs)
			if (!it.duration.empty())
			{
				int dw = 0, dh = 0;
				TTF_SizeUTF8(fSmall, it.duration.c_str(), &dw, &dh);
				int bx = tr.x + tr.w - dw - W * 1 / 100, by = tr.y + tr.h - dh - W * 1 / 200;
				box(bx - 6, by - 2, dw + 12, dh + 4, sk.bezel, SDL_Color { 0, 0, 0, 0 }, 0);
				drawText(it.duration, fSmall, sk.ink, bx, by);
			}
			// a Recent row's resume point, on the picture's top-left corner
			if (view == View::RECENT && it.resume > 0)
			{
				std::string rs = fmtTime(it.resume);
				int rw = 0, rh = 0;
				TTF_SizeUTF8(fSmall, rs.c_str(), &rw, &rh);
				box(tr.x + 4, tr.y + 4, rw + 12, rh + 4, sk.gold, SDL_Color { 0, 0, 0, 0 }, 0);
				drawText(rs, fSmall, sk.bg, tr.x + 10, tr.y + 6);
			}
			int textX = tr.x + tr.w + W * 2 / 100;
			int textW = W * 94 / 100 - textX;
			drawText(it.title, fMed, on ? sk.goldHi : sk.ink, textX, y + cardH * 12 / 100, textW);
			drawText(it.uploader, fSmall, sk.soft, textX, y + cardH * 55 / 100, textW);
		}
	}

	void drawChannels(int W, int H, const Skin& sk)
	{
		int top = resultsTop(H);
		int rowH = resultsRowH(H);
		int per = resultsPerPage();
		int gap = W * 1 / 100;
		int cardH = rowH - gap;
		int inset = std::max(4, W / 250);
		if (sel < scroll) scroll = sel;
		if (sel >= scroll + per) scroll = sel - per + 1;
		if (channels.empty())
		{
			drawText("No channels yet. Search for videos or play something.", fMed, sk.soft, W * 6 / 100, top);
			return;
		}
		int av = std::min(cardH - 2 * inset, H * 8 / 100);
		for (int i = scroll; i < static_cast<int>(channels.size()) && i < scroll + per; i++)
		{
			int y = top + (i - scroll) * rowH;
			bool on = i == sel;
			box(W * 4 / 100, y, W * 92 / 100, cardH, sk.card, on ? sk.gold : sk.line, sk.bw);
			if (on)
			{
				SDL_Rect bar { W * 4 / 100, y, std::max(6, W * 8 / 1000), cardH };
				SDL_SetRenderDrawColor(ren, sk.gold.r, sk.gold.g, sk.gold.b, 255);
				SDL_RenderFillRect(ren, &bar);
			}
			const std::string& name = channels[i];
			// a square avatar with the channel's first letter: no network round-trip for a channel row
			SDL_Rect ar { W * 6 / 100, y + (cardH - av) / 2, av, av };
			box(ar.x, ar.y, ar.w, ar.h, sk.ring, sk.edge, sk.bw);
			std::string initial = !name.empty() && static_cast<unsigned char>(name[0]) < 128
				? std::string(1, static_cast<char>(toupper(static_cast<unsigned char>(name[0])))) : "@";
			int iw = 0, ih = 0;
			TTF_SizeUTF8(fBig, initial.c_str(), &iw, &ih);
			drawText(initial, fBig, sk.gold, ar.x + (ar.w - iw) / 2, ar.y + (ar.h - ih) / 2);
			// how many of the two lists this channel appears in (unique videos)
			std::vector<std::string> seen;
			for (const Item& it : searchResults)
				if (it.uploader == name)
				{
					bool dup = false;
					for (const std::string& id : seen) if (id == it.id) dup = true;
					if (!dup) seen.push_back(it.id);
				}
			for (const Item& it : recent)
				if (it.uploader == name)
				{
					bool dup = false;
					for (const std::string& id : seen) if (id == it.id) dup = true;
					if (!dup) seen.push_back(it.id);
				}
			int tx = ar.x + ar.w + W * 2 / 100;
			int tw = W * 94 / 100 - tx;
			drawText(name, fMed, on ? sk.goldHi : sk.ink, tx, y + cardH * 12 / 100, tw);
			drawText(std::to_string(seen.size()) + (seen.size() == 1 ? " video" : " videos"), fSmall, sk.soft,
				tx, y + cardH * 55 / 100, tw);
		}
	}

};

int main(int argc, char** argv)
{
	App app;
	for (int i = 1; i < argc; i++)
	{
		std::string a = argv[i];
		if (a == "--font" && i + 1 < argc) { app.fontPath = argv[++i]; app.fontPathMed = app.fontPath; }
		else if (a == "--help") { printf("banana-tube [--font FILE]\n"); return 0; }
	}
	curl_global_init(CURL_GLOBAL_DEFAULT);
	mkdir("/data/banana/cache", 0755);
	mkdir(CACHE_DIR, 0755);
	app.recentLoad();
	app.qualityLoad();
	app.buildChannels();
	int rc = app.run();
	curl_global_cleanup();
	return rc;
}
