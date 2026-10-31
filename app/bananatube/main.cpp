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

// A stop asked from outside (the launcher's `kill` on the app's process group, a shutdown): the loop leaves and the
// mpv child is reaped, so the menu's launcher always returns and the menu comes back.
static volatile sig_atomic_t g_stop = 0;
static void onStopSignal(int) { g_stop = 1; }

namespace
{
	const char* DEFAULT_FONT = "/usr/share/banana/banana/fonts/Inter-Regular.ttf";
	const char* CACHE_DIR = "/data/banana/cache/tube";
	// YouTube's public web InnerTube key (the one its own web client ships): the search endpoint needs it. It is not
	// a secret and is not per-user; a failure just falls back to yt-dlp.
	const char* INNERTUBE_KEY = "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8";
	const char* INNERTUBE_SEARCH = "https://www.youtube.com/youtubei/v1/search";

	enum class Mode { KEYS, RESULTS, PLAYING };

	struct Item
	{
		std::string id, title, uploader, duration;
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

	Mode mode = Mode::KEYS;
	int keyRow = 1, keyCol = 0;
	bool upper = false;
	std::string query;
	std::string status;
	std::vector<Item> results;
	int sel = 0, scroll = 0;
	bool quit = false;					// the loop's own exit (B on an empty query): SDL_Quit() here would tear SDL
										// down under a running loop, leaving a frozen/glitched window that never exits
	Uint32 lastFingerAt = 0;			// a touch also arrives as a mouse click on some stacks: ignore that duplicate

	pid_t mpv = -1;
	int ipc = -1;

	std::mutex searchLock;
	std::thread searchThread;
	std::atomic<bool> searching { false };
	std::atomic<bool> searchDone { false };
	std::vector<Item> found;
	std::string searchErr;

	// thumbnails: downloaded to disk by one background worker, loaded into textures on the render thread, keyed by id
	std::map<std::string, SDL_Texture*> thumbTex;
	std::thread thumbThread;
	std::mutex thumbMx;
	std::condition_variable thumbCv;
	std::atomic<bool> thumbPending { false };
	bool thumbStop = false;

	// ---- text ----------------------------------------------------------------------------------------------------
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
		SDL_Texture* t = text(shown, f, col, &w, &h);
		if (!t) return;
		SDL_Rect r { x, y, w, h };
		SDL_RenderCopy(ren, t, nullptr, &r);
		SDL_DestroyTexture(t);
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
				std::string p = thumbPath(it.id);
				struct stat st;
				if (!stat(p.c_str(), &st) && st.st_size > 100) continue;
				std::string url = "https://i.ytimg.com/vi/" + it.id + "/mqdefault.jpg";
				bool ok = false;
				std::string bytes = curlBody(url, nullptr, &ok, 12);
				if (!ok || bytes.size() < 100) continue;
				std::string tmp = p + ".part";
				if (FILE* f = fopen(tmp.c_str(), "wb")) { fwrite(bytes.data(), 1, bytes.size(), f); fclose(f); rename(tmp.c_str(), p.c_str()); }
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
		if (query.empty() || searching) return;
		searchDone = false;
		searching = true;
		status = "Searching\u2026";
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
		if (searchThread.joinable()) searchThread.join();
		std::lock_guard<std::mutex> l(searchLock);
		searching = false;
		results = found;
		sel = scroll = 0;
		if (results.empty()) status = searchErr.empty() ? "No results" : searchErr;
		else status = std::to_string(results.size()) + " results";
		if (!results.empty())
		{
			mode = Mode::RESULTS;
			for (auto& kv : thumbTex) if (kv.second) SDL_DestroyTexture(kv.second);
			thumbTex.clear();
			prefetchThumbs();
		}
	}

	// ---- mpv -----------------------------------------------------------------------------------------------------
	void ipcSend(const std::string& command)
	{
		if (ipc < 0) return;
		std::string msg = command + "\n";
		if (write(ipc, msg.data(), msg.size()) < 0) {}
	}
	void connectIpc()
	{
		if (ipc >= 0) return;
		int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0) return;
		sockaddr_un sa {};
		sa.sun_family = AF_UNIX;
		snprintf(sa.sun_path, sizeof sa.sun_path, "%s", IPC_SOCK);
		for (int i = 0; i < 50; i++)
		{
			if (connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0) { ipc = fd; return; }
			usleep(100000);
		}
		close(fd);
	}
	void play(const Item& it)
	{
		if (mpv > 0) { status = "Already playing"; return; }
		unlink(IPC_SOCK);
		std::string url = "https://www.youtube.com/watch?v=" + it.id;
		const char* ytdlFormat =
			"bestvideo[height<=720][vcodec^=avc1]+bestaudio"
			"/bestvideo[height<=720][vcodec^=vp9]+bestaudio"
			"/bestvideo[height<=720][vcodec!*=av01]+bestaudio"
			"/best[height<=720][vcodec!*=av01]/best[height<=720]/best";
		const char* argv[] = {
			"mpv",
			"--no-config", "--no-terminal", "--input-default-bindings=no",
			"--input-ipc-server=" IPC_SOCK,
			"--vo=wlshm",							// stable software output (the GPU VO goes white here)
			"--hwdec=auto-copy",					// hardware decode where a copy-back path exists, else software
			"--ao=alsa", "--audio-format=s16",
			"--audio-buffer=0.4",					// small, matched to the AudioReach period
			"--cache=yes",							// stream from a small buffer so seeks/pauses do not stall
			// big, touch-friendly on-screen controls: mpv maps wl_touch to a click on its OSC, so these are the
			// player buttons the handheld's finger reaches during playback. osd-scale-by-window=no keeps them a fixed,
			// large size instead of scaling to the (already fixed) panel.
			"--osc=yes",
			"--fullscreen", "--keep-open=no", "--force-window=yes",
			"--msg-level=all=warn",
		};
		// The OSC's size is `osc-scalefullscreen` (a script-opt), NOT --osd-scale: the OSC ignores --osd-scale, which
		// is why the buttons stayed small and --osd-scale also left the OSC's hitboxes unmoved (touch looked offset).
		// Scaling the OSC itself keeps its drawing and its hit-testing in the same geometry. Tunable on the card
		// (default 2.0x) via /data/banana/bananatube/osd-scale.
		float osc = 2.0f;
		if (FILE* f = fopen("/data/banana/bananatube/osd-scale", "r")) { if (fscanf(f, "%f", &osc) != 1) {} fclose(f); }
		if (osc < 0.5f) osc = 0.5f;
		if (osc > 5.0f) osc = 5.0f;
		char oscbuf[112];
		snprintf(oscbuf, sizeof oscbuf, "--script-opts=osc-scalefullscreen=%.2f,osc-scalewindowed=%.2f", osc, osc);
		std::string fmt = std::string("--ytdl-format=") + ytdlFormat;
		std::vector<char*> av;
		for (const char* a : argv) av.push_back(const_cast<char*>(a));
		av.push_back(oscbuf);
		av.push_back(&fmt[0]);
		av.push_back(&url[0]);
		av.push_back(nullptr);
		logLine("playing " + url);
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
		status = "Playing: " + it.title;
	}
	// graceful stop: ask mpv to quit (so it releases ALSA cleanly), then TERM, then KILL. A hard kill mid-stream
	// leaves the AudioReach frontend to time out, which is the EBUSY the next open would hit.
	void stopPlayback()
	{
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
		else if (mode == Mode::RESULTS) { mode = Mode::KEYS; }
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
			if (sel >= 0 && sel < static_cast<int>(results.size())) play(results[sel]);
		}
		else if (mode == Mode::PLAYING)
		{
			connectIpc();
			ipcSend("{\"command\":[\"cycle\",\"pause\"]}");
		}
	}
	void move(int dx, int dy)
	{
		if (mode == Mode::KEYS)
		{
			if (keyRow >= ROWS) keyCol = (keyCol + dx + 4) % 4;
			else keyCol = (keyCol + dx + COLS) % COLS;
			if (dy != 0)
			{
				int nr = keyRow + dy;
				if (nr < 0) nr = ROWS;
				if (nr > ROWS) nr = 0;
				keyRow = nr;
				keyCol = 0;
			}
		}
		else if (mode == Mode::RESULTS)
		{
			int n = static_cast<int>(results.size());
			if (n == 0) return;
			sel += dy != 0 ? dy : dx;
			if (sel < 0) sel = 0;
			if (sel >= n) sel = n - 1;
		}
		else if (mode == Mode::PLAYING)
		{
			connectIpc();
			if (dy != 0) ipcSend(dy < 0 ? "{\"command\":[\"seek\",\"10\"]}" : "{\"command\":[\"seek\",\"-10\"]}");
			if (dx != 0) ipcSend(dx < 0 ? "{\"command\":[\"seek\",\"-60\"]}" : "{\"command\":[\"seek\",\"60\"]}");
		}
	}
	void page(int dir)
	{
		if (mode != Mode::RESULTS) return;
		int n = static_cast<int>(results.size()), step = resultsPerPage();
		if (n == 0) return;
		sel += dir * step;
		if (sel < 0) sel = 0;
		if (sel >= n) sel = n - 1;
	}

	// ---- shared layout geometry (draw and touch use the SAME rects, so a tap can never land beside what is drawn) --
	int kTop(int H) const { return H * 17 / 100; }
	SDL_Rect keyRect(int W, int H, int r, int c) const
	{
		int kw = W * 9 / 100, kh = H * 11 / 100, gap = W * 1 / 100;
		int rowW = COLS * kw + (COLS - 1) * gap, x0 = (W - rowW) / 2;
		return SDL_Rect { x0 + c * (kw + gap), kTop(H) + r * (kh + gap), kw, kh };
	}
	SDL_Rect actRect(int W, int H, int c) const
	{
		int gap = W * 1 / 100, kh = H * 11 / 100, aw = W * 21 / 100, ah = H * 11 / 100;
		int rowW = 4 * aw + 3 * gap, x0 = (W - rowW) / 2, y = kTop(H) + ROWS * (kh + gap);
		return SDL_Rect { x0 + c * (aw + gap), y, aw, ah };
	}
	int resultsTop(int H) const { return H * 16 / 100; }
	int resultsRowH(int H) const { return std::max(56, H * 13 / 100); }
	static bool inRect(const SDL_Rect& r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

	// a tap (touch or pointer) at output-space (x,y): the top bar's Back / query, then the current screen's targets
	void tapAt(int x, int y)
	{
		if (getenv("BANANA_TUBE_TOUCH_DEBUG")) { char b[64]; snprintf(b, sizeof b, "tap %d %d (mode %d)", x, y, (int)mode); logLine(b); }
		int W = 0, H = 0;
		SDL_GetRendererOutputSize(ren, &W, &H);
		SDL_Rect backBtn { W * 3 / 100, H * 4 / 100, W * 11 / 100, H * 9 / 100 };
		SDL_Rect queryBox { W * 15 / 100, H * 4 / 100, W * 81 / 100, H * 9 / 100 };
		if (inRect(backBtn, x, y)) { logLine("tap -> Back"); back(); return; }
		if (inRect(queryBox, x, y)) { logLine("tap -> query box"); if (mode != Mode::PLAYING) mode = Mode::KEYS; return; }
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
			activate();	// mpv owns the screen here; this only matters if its window is not covering ours
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
	bool tDown = false, tWasDown = false, tPos = false, tDefer = false;	// tPos: a position arrived; tDefer: waited one report
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
			if (bit(ABS_MT_POSITION_X) && bit(ABS_MT_POSITION_Y))
			{
				input_absinfo ax = {}, ay = {};
				ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax);
				ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay);
				tMinX = ax.minimum; tMaxX = ax.maximum; tMinY = ay.minimum; tMaxY = ay.maximum;
				tX = (tMinX + tMaxX) / 2; tY = (tMinY + tMaxY) / 2;
				touchFd = fd;
				break;
			}
			::close(fd);
		}
		closedir(d);
	}
	void mapTouch(int& x, int& y, int W, int H)
	{
		float sx = tMaxX > tMinX ? (float)panelW / (tMaxX - tMinX + 1) : 1;
		float sy = tMaxY > tMinY ? (float)panelH / (tMaxY - tMinY + 1) : 1;
		float px = (tX - tMinX) * sx, py = (tY - tMinY) * sy;
		int rx, ry;
		switch (panelRot)
		{
		case 90:  rx = (int)py; ry = (int)(panelW - 1 - px); break;
		case 270: rx = (int)(panelH - 1 - py); ry = (int)px; break;
		case 180: rx = (int)(panelW - 1 - px); ry = (int)(panelH - 1 - py); break;
		default:  rx = (int)px; ry = (int)py; break;
		}
		if (flipX) rx = W - 1 - rx;
		if (flipY) ry = H - 1 - ry;
		x = rx; y = ry;
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
			for (int i = 0; i < (int)(n / sizeof(input_event)); i++)
			{
				const input_event& e = ev[i];
				if (e.type == EV_ABS && (e.code == ABS_MT_POSITION_X || e.code == ABS_X)) { tX = e.value; tPos = true; }
				else if (e.type == EV_ABS && (e.code == ABS_MT_POSITION_Y || e.code == ABS_Y)) { tY = e.value; tPos = true; }
				else if (e.type == EV_ABS && e.code == ABS_MT_TRACKING_ID && e.value < 0) tDown = false;
				else if (e.type == EV_KEY && e.code == BTN_TOUCH) tDown = e.value != 0;
				else if (e.type == EV_SYN && e.code == SYN_REPORT)
				{
					if (!tDown) { tWasDown = false; tPos = false; tDefer = false; }	// released
					else if (!tWasDown)
					{
						// a down: wait one report for the new position (the driver may send BTN_TOUCH before the point),
						// then fire even without it (a repeat tap at the same spot sends no new position)
						if (tPos || tDefer)
						{
							int x = 0, y = 0;
							mapTouch(x, y, W, H);
							{ char b[96]; snprintf(b, sizeof b, "tap raw %d,%d -> %d,%d (surface %dx%d rot %d)", tX, tY, x, y, W, H, panelRot); logLine(b); }
							// during playback mpv's fullscreen window owns the screen and reads touch itself (its big OSC)
							if (mode != Mode::PLAYING) tapAt(x, y);
							tWasDown = true; tPos = false; tDefer = false;
						}
						else tDefer = true;
					}
				}
			}
		}
	}

	// ---- the loop ------------------------------------------------------------------------------------------------
	int run()
	{
		SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
		TTF_Init();
		IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
		SDL_GameControllerEventState(SDL_ENABLE);
		for (int i = 0; i < SDL_NumJoysticks(); i++)
			if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); if (pad) break; }

		win = SDL_CreateWindow("BananaTube", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 768,
			SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
		if (!win) { logLine(std::string("no window: ") + SDL_GetError()); return 1; }
		ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
		if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
		if (!ren) { logLine(std::string("no renderer: ") + SDL_GetError()); return 1; }
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

		fBig = TTF_OpenFont(fontPath.c_str(), 40);
		fMed = TTF_OpenFont(fontPath.c_str(), 30);
		fSmall = TTF_OpenFont(fontPath.c_str(), 22);
		if (!fBig || !fMed || !fSmall) { logLine(std::string("no font ") + fontPath + ": " + TTF_GetError()); return 1; }
		{
			int W = 0, H = 0;
			SDL_GetRendererOutputSize(ren, &W, &H);
			if (H > 0)
			{
				TTF_CloseFont(fBig); TTF_CloseFont(fMed); TTF_CloseFont(fSmall);
				fBig = TTF_OpenFont(fontPath.c_str(), std::max(16, H * 5 / 100));
				fMed = TTF_OpenFont(fontPath.c_str(), std::max(13, H * 4 / 100));
				fSmall = TTF_OpenFont(fontPath.c_str(), std::max(10, H * 3 / 100));
				if (!fBig || !fMed || !fSmall) { logLine("no font at the scaled size"); return 1; }
			}
		}

		if (std::getenv("BANANA_TUBE_SELFTEST")) return selftest();

		signal(SIGTERM, onStopSignal);
		signal(SIGINT, onStopSignal);
		signal(SIGHUP, onStopSignal);

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
					case SDL_CONTROLLER_BUTTON_X: if (mode == Mode::KEYS) { if (!query.empty()) query.pop_back(); } else page(-1); break;
					case SDL_CONTROLLER_BUTTON_Y: if (mode == Mode::KEYS) upper = !upper; else page(1); break;
					case SDL_CONTROLLER_BUTTON_START: if (mode != Mode::PLAYING) startSearch(); break;
					case SDL_CONTROLLER_BUTTON_BACK: if (mode == Mode::PLAYING) activate(); break;	// Select: pause
					case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: page(-1); break;
					case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: page(1); break;
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
					else if (k >= 32 && k < 127 && mode == Mode::KEYS) query += static_cast<char>(k);
				}
				// touch: SDL_FINGER coords are normalized to the window (multiply by the output size); a stack that has
				// no SDL touch support delivers the tap as a left mouse click in window coords, which we scale to the
				// renderer's output size. Either way the point is the one tapAt() hit-tests against the drawn rects.
				else if (ev.type == SDL_FINGERDOWN)
				{
					int W = 0, H = 0;
					SDL_GetRendererOutputSize(ren, &W, &H);
					lastFingerAt = SDL_GetTicks();
					tapAt((int)(ev.tfinger.x * W), (int)(ev.tfinger.y * H));
				}
				else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT)
				{
					if (SDL_GetTicks() - lastFingerAt > 400)	// not the pointer emulated from the same touch
					{
						int W = 0, H = 0, winW = 0, winH = 0;
						SDL_GetRendererOutputSize(ren, &W, &H);
						SDL_GetWindowSize(win, &winW, &winH);
						tapAt(winW > 0 ? ev.button.x * W / winW : ev.button.x, winH > 0 ? ev.button.y * H / winH : ev.button.y);
					}
				}
			}

			Uint32 now = SDL_GetTicks();
			if (pad)
			{
				int dx = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) - SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
				int dy = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) - SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP);
				if (dx != heldX || dy != heldY) { heldX = dx; heldY = dy; lastRepeat = now; }
				else if ((dx || dy) && now - lastRepeat > 250) { move(dx, dy); lastRepeat = now + 60; }
			}

			pollSearch();
			pollTouch();
			if (mode == Mode::PLAYING && !mpvRunning()) { mode = Mode::RESULTS; status = "Playback ended (B to go back)"; }

			draw();
			SDL_RenderPresent(ren);
			SDL_Delay(8);
		}
		stopPlayback();
		if (searchThread.joinable()) searchThread.join();
		{ std::lock_guard<std::mutex> l(thumbMx); thumbStop = true; }
		thumbCv.notify_all();
		if (thumbThread.joinable()) thumbThread.join();
		if (touchFd >= 0) { close(touchFd); touchFd = -1; }
		for (auto& kv : thumbTex) if (kv.second) SDL_DestroyTexture(kv.second);
		return 0;
	}

	// a headless pass over the pure logic (the harness has no display): the InnerTube walk, the yt-dlp line split,
	// the keyboard navigation and the back()/activate() state machine.
	int selftest()
	{
		Item it;
		bool ok = splitLine("abc123\tA Title\tSome One\t3:21", it) && it.id == "abc123" && it.duration == "3:21";
		printf("bananatube selftest split: %s\n", ok ? "ok" : "fail");
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
		// the touch transform: raw (1077,25) on the 1080x1240 panel, turned 90, is the app surface's top-left (25,2)
		tMinX = 0; tMaxX = 1079; tMinY = 0; tMaxY = 1239; panelRot = 90; flipX = flipY = false;
		tX = 1077; tY = 25;
		int mx = 0, my = 0;
		mapTouch(mx, my, 1240, 1080);
		printf("bananatube selftest touch: %d,%d (expect 25,2)\n", mx, my);
		return 0;
	}

	int resultsPerPage() const
	{
		int W = 0, H = 0;
		SDL_GetRendererOutputSize(ren, &W, &H);
		int per = (H * 74 / 100) / resultsRowH(H);
		return per < 1 ? 1 : per;
	}

	// ---- drawing -------------------------------------------------------------------------------------------------
	void draw()
	{
		int W = 0, H = 0;
		SDL_GetRendererOutputSize(ren, &W, &H);
		SDL_SetRenderDrawColor(ren, 12, 14, 18, 255);
		SDL_RenderClear(ren);
		SDL_Color fg { 236, 238, 242, 255 }, dim { 150, 156, 166, 255 }, acc { 224, 64, 47, 255 }, selbg { 40, 46, 58, 255 };

		// the top bar: a Back button (B's target) and the query box (a tap reopens the keyboard). The rects are the
		// same ones tapAt() hit-tests, so a finger lands on exactly what is drawn.
		SDL_Rect back { W * 3 / 100, H * 4 / 100, W * 11 / 100, H * 9 / 100 };
		SDL_Rect queryBox { W * 15 / 100, H * 4 / 100, W * 81 / 100, H * 9 / 100 };
		panel(back.x, back.y, back.w, back.h, SDL_Color { 30, 34, 44, 255 }, SDL_Color { 60, 66, 80, 255 });
		int bw = 0, bh = 0;
		TTF_SizeUTF8(fMed, "Back", &bw, &bh);
		drawText("Back", fMed, fg, back.x + (back.w - bw) / 2, back.y + (back.h - bh) / 2);
		panel(queryBox.x, queryBox.y, queryBox.w, queryBox.h, SDL_Color { 22, 26, 34, 255 }, SDL_Color { 60, 66, 80, 255 });
		std::string shown = query.empty() ? "Type a search\u2026" : query;
		drawText(shown, fMed, query.empty() ? dim : fg, queryBox.x + W * 2 / 100, queryBox.y + (queryBox.h - bh) / 2, queryBox.w - W * 4 / 100);
		if (!status.empty()) drawText(status, fSmall, dim, W * 4 / 100, H * 14 / 100, W * 92 / 100);

		if (mode == Mode::KEYS) drawKeyboard(W, H, fg, dim, acc, selbg);
		else drawResults(W, H, fg, dim, acc, selbg);

		SDL_Color hint { 120, 126, 138, 255 };
		const char* help = mode == Mode::PLAYING ? "A play/pause   d-pad seek   B stop"
			: mode == Mode::RESULTS ? "A play   B back   L/R page   START new search"
			: "A type   B back   START search   Y shift";
		drawText(help, fSmall, hint, W * 4 / 100, H * 94 / 100, W * 92 / 100);
	}

	void drawKeyboard(int W, int H, SDL_Color fg, SDL_Color dim, SDL_Color acc, SDL_Color selbg)
	{
		const char* (*keys)[11] = upper ? UPPER : LOWER;
		for (int r = 0; r < ROWS; r++)
			for (int c = 0; c < COLS; c++)
			{
				const char* k = keys[r][c];
				SDL_Rect rr = keyRect(W, H, r, c);
				bool on = keyRow == r && keyCol == c;
				panel(rr.x, rr.y, rr.w, rr.h, on ? selbg : SDL_Color { 24, 28, 36, 255 }, on ? acc : SDL_Color { 54, 60, 74, 255 }, on ? 1 : 0);
				int tw = 0, th = 0;
				TTF_SizeUTF8(fMed, k, &tw, &th);
				drawText(k, fMed, on ? fg : dim, rr.x + (rr.w - tw) / 2, rr.y + (rr.h - th) / 2);
			}
		const char* acts[] = { "Space", "Del", "Shift", "Search" };
		for (int c = 0; c < 4; c++)
		{
			SDL_Rect rr = actRect(W, H, c);
			bool on = keyRow >= ROWS && keyCol == c;
			SDL_Color line = on ? acc : SDL_Color { 54, 60, 74, 255 };
			panel(rr.x, rr.y, rr.w, rr.h, on ? selbg : SDL_Color { 24, 28, 36, 255 }, line, on ? 1 : 0);
			int tw = 0, th = 0;
			TTF_SizeUTF8(fMed, acts[c], &tw, &th);
			drawText(acts[c], fMed, on ? fg : dim, rr.x + (rr.w - tw) / 2, rr.y + (rr.h - th) / 2);
		}
	}

	void drawResults(int W, int H, SDL_Color fg, SDL_Color dim, SDL_Color acc, SDL_Color selbg)
	{
		int top = resultsTop(H);
		int rowH = resultsRowH(H);
		int per = resultsPerPage();
		if (sel < scroll) scroll = sel;
		if (sel >= scroll + per) scroll = sel - per + 1;
		if (searching || results.empty())
		{
			drawText(searching ? "Searching\u2026" : "No results. B for the keyboard.", fMed, dim, W * 6 / 100, top);
			return;
		}
		int pad = W * 1 / 100;
		int thumbH = rowH - 2 * pad, thumbW = thumbH * 16 / 9;
		if (thumbW > W * 30 / 100) { thumbW = W * 30 / 100; thumbH = thumbW * 9 / 16; }
		for (int i = scroll; i < static_cast<int>(results.size()) && i < scroll + per; i++)
		{
			const Item& it = results[i];
			int y = top + (i - scroll) * rowH;
			bool on = i == sel;
			panel(W * 4 / 100, y, W * 92 / 100, rowH - pad, on ? selbg : SDL_Color { 20, 24, 31, 255 },
				on ? acc : SDL_Color { 46, 52, 64, 255 }, on ? 1 : 0);
			int tx = W * 6 / 100;
			// the thumbnail (a 16:9 card at the left), or a placeholder until it is downloaded
			SDL_Rect tr { tx, y + (rowH - pad - thumbH) / 2, thumbW, thumbH };
			SDL_Texture* t = thumb(it.id);
			if (t) SDL_RenderCopy(ren, t, nullptr, &tr);
			else
			{
				SDL_SetRenderDrawColor(ren, 34, 38, 48, 255);
				SDL_RenderFillRect(ren, &tr);
				SDL_SetRenderDrawColor(ren, 54, 60, 74, 255);
				SDL_RenderDrawRect(ren, &tr);
			}
			int textX = tr.x + tr.w + W * 2 / 100;
			int textW = W * 88 / 100 - textX;
			// the duration badge, right-aligned before the ring
			if (!it.duration.empty())
			{
				int dw = 0, dh = 0;
				TTF_SizeUTF8(fSmall, it.duration.c_str(), &dw, &dh);
				int bx = W * 94 / 100 - dw - W * 2 / 100, by = y + (rowH - pad) / 2 - dh / 2;
				panel(bx - 6, by - 2, dw + 12, dh + 4, SDL_Color { 8, 9, 12, 220 }, SDL_Color { 0, 0, 0, 0 });
				drawText(it.duration, fSmall, fg, bx, by);
				textW -= dw + W * 4 / 100;
			}
			drawText(it.title, fMed, fg, textX, y + rowH * 12 / 100, textW);
			drawText(it.uploader, fSmall, dim, textX, y + rowH * 55 / 100, textW);
		}
	}
};

int main(int argc, char** argv)
{
	App app;
	for (int i = 1; i < argc; i++)
	{
		std::string a = argv[i];
		if (a == "--font" && i + 1 < argc) app.fontPath = argv[++i];
		else if (a == "--help") { printf("banana-tube [--font FILE]\n"); return 0; }
	}
	curl_global_init(CURL_GLOBAL_DEFAULT);
	mkdir("/data/banana/cache", 0755);
	mkdir(CACHE_DIR, 0755);
	int rc = app.run();
	curl_global_cleanup();
	return rc;
}
