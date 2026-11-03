#include "sound.h"
#include <unistd.h>
#include "../rnds/RndsUI.h"
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef BANANA_ALSA
#include <alsa/asoundlib.h>
#include <sys/stat.h>
#endif
#include <time.h>

namespace banana
{

	// 16-bit PCM WAV only (what rnds/gen_sounds.py writes)
	static bool loadWav(const std::string& path, std::vector<int16_t>& pcm, int& rate, int& channels)
	{
		FILE* f = fopen(path.c_str(), "rb");
		if (!f) return false;
		std::vector<uint8_t> b;
		uint8_t buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
		fclose(f);
		if (b.size() < 44 || memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4)) return false;
		size_t at = 12;
		int bits = 0;
		while (at + 8 <= b.size())
		{
			uint32_t len = b[at + 4] | (b[at + 5] << 8) | (b[at + 6] << 16) | ((uint32_t)b[at + 7] << 24);
			const uint8_t* c = b.data() + at + 8;
			if (!memcmp(b.data() + at, "fmt ", 4))
			{
				// the header says 16 bytes, but the file can be cut short inside it: check the body, not just the size
				if (len < 16 || at + 8 + len > b.size()) return false;
				channels = c[2] | (c[3] << 8);
				rate = c[4] | (c[5] << 8) | (c[6] << 16) | ((uint32_t)c[7] << 24);
				bits = c[14] | (c[15] << 8);
			}
			else if (!memcmp(b.data() + at, "data", 4))
			{
				if (bits != 16 || channels < 1 || channels > 8 || rate <= 0) return false;
				len = (uint32_t)std::min<size_t>(len, b.size() - at - 8);
				pcm.resize(len / 2);
				memcpy(pcm.data(), c, pcm.size() * 2);
				return true;
			}
			at += 8 + len + (len & 1);
		}
		return false;
	}

#ifdef BANANA_ALSA
	// A log line prefixed with the seconds since boot, so the sound's opens and cues can be lined up with the kernel's
	// (bananad's and the menad's boot marks, dmesg's amp start failures).
	static void slog(const char* fmt, ...)
	{
		char msg[256];
		va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
		double up = 0; FILE* f = fopen("/proc/uptime", "r");
		if (f) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }
		fprintf(stderr, "banana-ui: sound: %6.2f s: %s\n", up, msg);
	}

	// What the sound device is doing, for bananad's sleep note (logs/sleep-state): a sleep whose resume never comes
	// back leaves no log line of its own, so the one thing the next boot can still read is whether a PCM was open --
	// or was closing -- at the moment the kernel went under. Overwritten on every transition; never read here.
	static void publishState(const char* what)
	{
		FILE* f = fopen("/run/banana/sound-state", "w");
		if (!f) return;
		double up = 0; FILE* u = fopen("/proc/uptime", "r");
		if (u) { if (fscanf(u, "%lf", &up) != 1) up = 0; fclose(u); }
		fprintf(f, "%s at %6.2f s\n", what, up);
		fclose(f);
	}

	// The menu plays on the same device as the games, and the card's output is a 48 kHz part: anything else
	// is resampled there by alsa-lib's linear converter (the image has no speexrate). Convert each clip once, here,
	// so the stream is always 48 kHz stereo and no plug resampler is involved. Mono (and >2-channel) clips are made
	// stereo too, so a cue and the ambient loop can always share one stream.
	static void to48k(std::vector<int16_t>& pcm, int& rate, int& channels)
	{
		if (rate == 48000) return;	// (channels are fixed up by loadWav below)
		if (channels < 1 || pcm.size() / channels < 2) return;
		size_t frames = pcm.size() / channels, n = (size_t)((double)frames * 48000 / rate);
		std::vector<int16_t> out(n * channels);
		for (size_t i = 0; i < n; i++)
		{
			double x = (double)i * rate / 48000;
			size_t j = (size_t)x;
			double f = x - j;
			if (j + 1 >= frames) { j = frames - 2; f = 1; }
			for (int c = 0; c < channels; c++)
				out[i * channels + c] = (int16_t)(pcm[j * channels + c] + (pcm[(j + 1) * channels + c] - pcm[j * channels + c]) * f);
		}
		pcm.swap(out);
		rate = 48000;
	}

	// every clip the menu plays is stereo (the mixer below writes one stream for the device)
	static void toStereo(std::vector<int16_t>& pcm, int& channels)
	{
		if (channels == 2 || channels < 1 || pcm.size() / channels < 1) return;
		size_t frames = pcm.size() / channels;
		std::vector<int16_t> out(frames * 2);
		for (size_t i = 0; i < frames; i++)
			for (int c = 0; c < 2; c++)
				out[i * 2 + c] = pcm[i * channels + (c < channels ? c : 0)];
		pcm.swap(out);
		channels = 2;
	}
#endif

	Sound::Sound(const std::string& dir)
	{
		for (const char* n : { "scroll", "select", "back", "fav", "ready", "insert" })
		{
			// a user theme's own sound for it (rnds::themeFiles; "off": none), else the theme's (one that doesn't
			// load: the theme's)
			auto own = rnds::themeFiles.sounds.find(n);
			if (own != rnds::themeFiles.sounds.end() && own->second == "off") continue;
			Clip c;
			bool got = own != rnds::themeFiles.sounds.end() && loadWav(own->second, c.pcm, c.rate, c.channels);
			if (!got) got = loadWav(dir + "/" + n + ".wav", c.pcm, c.rate, c.channels);
			if (got)
			{
#ifdef BANANA_ALSA
				toStereo(c.pcm, c.channels);
				to48k(c.pcm, c.rate, c.channels);
#endif
				clips[n] = std::move(c);
			}
		}
		// the ambient background loop: a WAV on the card (bgm.wav, or music/bgm.wav), else the image's own default.
		// Written by the user; the settings screen shows "Background Music" only when one is found.
		for (const char* p : { "/data/bgm.wav", "/data/music/bgm.wav", "/usr/share/banana/bgm.wav" })
			if (loadWav(p, bgm.pcm, bgm.rate, bgm.channels)) { bgmLoaded = true; break; }
#ifdef BANANA_ALSA
		if (bgmLoaded) { toStereo(bgm.pcm, bgm.channels); to48k(bgm.pcm, bgm.rate, bgm.channels); }
		bgmStartAt = std::chrono::steady_clock::now();	// setBgm(true) starts the loop from here
		publishState("closed (startup)");
#endif
		th = std::thread([this] { run(); });
	}

	Sound::~Sound()
	{
		{
			std::lock_guard<std::mutex> l(m);
			stop = true;
		}
		cv.notify_all();
		if (th.joinable()) th.join();
	}

	void Sound::play(const std::string& name)
	{
		if (!enabled || !clips.count(name)) return;
		{
			std::lock_guard<std::mutex> l(m);
			queued = name;
		}
		cv.notify_all();
	}

	/* The boot warm-up: open the device now, independent of the background loop, and hold it until the first cue.
	 * The splash's insert is ~10 s after the kernel started, when the AudioReach card is not openable yet; the cue's
	 * own open then fails. The runner retries until the card comes up and holds the stream open (silence) ready for
	 * it. Cleared by the first cue, and by a deadline so a splash with no cue can't hold the device forever. */
	void Sound::warm()
	{
		if (!enabled) return;
		warmAt = std::chrono::steady_clock::now();	// the hold's deadline starts now, not at the last cue
		handedOver = false;	// a wake (bananad's SIGUSR1, the guard's press) hands the device back to the menu
		warmUp = true;
#ifdef BANANA_ALSA
		slog("warm(): the first cue will find the device open");
#endif
		cv.notify_all();
	}

	/* idle.h's burn-in guard calls this: the stream lives exactly while the screen is on. Blanking the panel is the
	 * real idle, so the device can go then -- and coming back from it pre-opens the stream, so the next cue is
	 * instant. The wake press itself is swallowed by the guard, which is the head-start. */
	void Sound::setScreenOn(bool on)
	{
		if (!enabled) return;
		if (on) { warm(); return; }
		{
			std::lock_guard<std::mutex> l(m);
			screenOff = true;
		}
		cv.notify_all();
	}

	void Sound::setBgm(bool on)	{
		if (on) handedOver = false;	// the menu is back (a game ended, or bananad woke the panel)
		bgmOn = on && bgmLoaded;
		/* The amp's PLL and the DSP graph need a moment after the card comes up (dmesg: start failure (-1)
		 * every few seconds until the amp locks), and the first few seconds of a boot are when the splash
		 * plays. The loop opens the device as soon as the setting turns on -- writes retry in place while the
		 * amp settles (writeFrames), so the device is warm and ready by the time the splash cue arrives -- and
		 * a closed device keeps the stream open for it instead of closing after a quiet spell. */
		if (on && bgmLoaded && !devOpen.load()) bgmStartAt = std::chrono::steady_clock::now();
#ifdef BANANA_ALSA
		slog("setBgm(%d) -> bgmOn=%d (loaded=%d)", on ? 1 : 0, bgmOn.load() ? 1 : 0, bgmLoaded ? 1 : 0);
#endif
		cv.notify_all();	// a closed device has to know there is a loop to open for
	}

	void Sound::setBgmGain(int permille)
	{
		if (permille < 0) permille = 0;
		if (permille > 1000) permille = 1000;
		bgmGain = permille;
	}

	void Sound::release()
	{
		{
			std::lock_guard<std::mutex> l(m);
#ifdef BANANA_ALSA
			slog("release(): the device is handed over");
			publishState("releasing (handover)");	// bananad's sleep note: a close still in flight here
#endif
			closeNow = true;
			handedOver = true;	// and stay closed: the loop must not reopen for the warm-up hold while it sleeps
			warmUp = false;
			queued.clear();
			bgmOn = false;	// the game takes the device; main.cpp calls setBgm(true) again when it comes back
		}
		/* the DSP gives one stream per frontend: the game that is about to start opens the same one, so wait for the
		 * thread to close it (it does within a frame or two) instead of racing it -- RetroArch's ALSA driver asserts
		 * on a handle it couldn't open */
		cv.notify_all();	// wake the thread if it is waiting for a cue with the device closed
		std::unique_lock<std::mutex> l(m);
		if (!cv.wait_for(l, std::chrono::seconds(2), [this] { return !closeNow; }))
		{
#ifdef BANANA_ALSA
			slog("release(): the device did not close in 2 s");
#endif
		}
#ifdef BANANA_ALSA
		/* the ack bananad waits for before it sleeps. A device that was already closed publishes nothing else (the
		 * loop's closeDev only runs when it has a handle), which left the note on "releasing" for good. */
		if (!devOpen.load()) publishState("closed (release)");
#endif
	}

	void Sound::run()
	{
#ifdef BANANA_ALSA
		// While the device is open the stream never stops: after a sound, silence follows. The DSP's output is
		// started when the stream powers up, and the first try fails because the I2S clock isn't running yet; a
		// running stream lets the retry find its clock. Writing 5 ms at a time also lets a new sound cut the one
		// playing (a held direction scrolls fast) within the buffer. QUIET is the boot warm-up's deadline: it holds
		// the device open for the first cue, then stops.
		const auto QUIET = std::chrono::seconds(20);
		snd_pcm_t* pcm = nullptr;
		int openRate = 0, openCh = 0;
		std::vector<int16_t> silence, mix;
		auto closeDev = [&](const char* why) { if (pcm) { slog("closing (%s)", why); snd_pcm_drop(pcm); snd_pcm_close(pcm); pcm = nullptr; devOpen = false; char s[64]; snprintf(s, sizeof s, "closed (%s)", why); publishState(s); } };
		// Where sound plays (speakers or headphones) is the sound server's now: ACP/WirePlumber follow the jack,
		// and bananad only keeps ALSA's "default" pointed at PipeWire in /run/banana/asound.conf (read via
		// /etc/asound.conf), so there is nothing here to reopen when the jack moves.

		/* The stream stays open between cues: closing it shortly after each one (150 ms, once) cost a whole
		 * graph instantiation per button press, so a single press was silent and only a fast run of them -- which
		 * kept the device open across the gaps -- was heard. The menu still releases it when a game starts and
		 * when the burn-in guard blanks the panel (Sound::release()). */
		const auto quiet = QUIET;
		/* A game's release() hands the device over and the menu reopens it on the way back. WirePlumber can wedge
		 * then: snd_pcm_open("default") fails with EBUSY on every retry (the sink is present and the default is
		 * right; restarting WirePlumber alone clears it -- measured on the unit). Count the failures and, once they
		 * are clearly not transient, ask the sound server to restart its session manager. Throttled. */
		int openFails = 0;
		auto lastRecover = std::chrono::steady_clock::now();
		auto openFail = [&]() {
			if (++openFails < 40) return;	// ~10 s of retries at 250 ms
			openFails = 0;
			auto t = std::chrono::steady_clock::now();
			if (t - lastRecover < std::chrono::seconds(20)) return;
			lastRecover = t;
			slog("the device will not open; asking the sound server to recover");
			if (std::system("/usr/libexec/banana/sound-server recover >/dev/null 2>&1 &") != 0) {}
		};
		/* The DSP's route (which frontend, which codec path, the FE<->BE switches) belongs to the sound server now:
		 * ACP applies the UCM profile and PipeWire re-applies the verb right before it opens the PCM (the UCM-open
		 * patch), and RetroArch is itself a PipeWire client. So this opens `default` -- the server -- directly, with
		 * no route write, frontend handover or settle: the menu used to do all of that only because it owned the
		 * frontend, and it was seconds of delay after every game. A jack move is ACP's too, so nothing here reopens
		 * the device for it. */
		auto openDev = [&](const Clip& c) -> bool
		{
			if (closeNow || handedOver.load()) return false;	// released: the game or the sleep holds the device
			slog("opening the device");
			if (closeNow) return false;
			/* alsa-lib reads /etc/asound.conf once per process. The menu starts before bananad has written the
			 * PipeWire default, so without this it kept ALSA's hardware default (card 0) for good: it held the Speaker
			 * frontend itself, and after every game the sound server and the menu fought over it. Re-read the config
			 * for each open so "default" is always the server. */
			snd_config_update_free_global();
			int e = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
			if (e < 0) { slog("open failed: %s", snd_strerror(e)); pcm = nullptr; openFail(); return false; }
			if (closeNow) { snd_pcm_close(pcm); pcm = nullptr; return false; }	// release() arrived during the open
			/* The host buffer: as small as the DSP will take, so a cue starts as soon as the mixer has it. A 128 ms
			 * buffer is refused outright on this card (no sound at all, docs/RPC-PORT.md), so try 15 ms and 20 ms
			 * before the 30 ms that has always worked; what took is logged. */
			int us = 0;
			for (int tryUs : { 15000, 20000, 30000 })
				if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, c.channels, c.rate, 1, (unsigned)tryUs) >= 0)
				{ us = tryUs; break; }
			if (!us)
			{
				slog("set_params failed"); snd_pcm_close(pcm); pcm = nullptr; openFail(); return false;
			}
			openRate = c.rate; openCh = c.channels;
			devOpen = true;
			openFails = 0;
			slog("device open, %d Hz, %d ch, %d ms buffer", openRate, openCh, us / 1000);
			{
				char s[64];
				snprintf(s, sizeof s, "open, %d Hz, %d ch, %d ms buffer", openRate, openCh, us / 1000);
				publishState(s);
			}
			silence.assign((size_t)(openRate / 200) * openCh, 0);
			return true;
		};
		auto writeFrames = [&](const int16_t* p, size_t frames) -> bool
		{
			int giveUp = 0;
			while (frames > 0)
			{
				snd_pcm_sframes_t w = snd_pcm_writei(pcm, p, (snd_pcm_uframes_t)frames);	// blocks while the buffer is full
				if (w < 0)
				{
					/* The AW88166's PLL and the DSP graph need a moment after the card comes up (dmesg: start
					 * failure (-1) every few seconds until the amp locks), and every failed write is reported
					 * here. Closing the device on one tears the DSP's graph down and rebuilds it, which is the
					 * choppy sound: the graph is gone for as long as the amp is failing. Prepare the stream in
					 * place and keep feeding it silence until it takes; the write succeeds once the PLL locks. */
					if (++giveUp > 20) { slog("write failed repeatedly (%s); giving up", snd_strerror((int)w)); return false; }
					slog("write failed (%s)", snd_strerror((int)w));
					int r = snd_pcm_recover(pcm, (int)w, 1);
					if (r < 0)
					{
						/* During boot, the Qualcomm AudioReach DSP and AW88166 amp hardware take ~1-2 seconds
						 * to finish DAPM power-up. An initial write during this window receives -EIO and
						 * recover/prepare returns -EBUSY. Do NOT close the device (which destroys the DSP graph
						 * and forces another multi-second teardown/rebuild cycle); sleep in short increments
						 * and retry snd_pcm_prepare() until the hardware finishes powering up. */
						for (int retry = 0; retry < 25 && r < 0; retry++)
						{
							std::this_thread::sleep_for(std::chrono::milliseconds(100));
							r = snd_pcm_prepare(pcm);
							// every 5th only: at boot the amps are not up for ~10 s and this retries the whole time,
							// which was a wall of one line per 100 ms in the log (25 lines per attempt)
							if (r < 0 && retry % 5 == 4) slog("prepare retry %d/25 (%s)", retry + 1, snd_strerror(r));
						}
					}
					if (r < 0) { slog("recover/prepare failed (%s); giving up", snd_strerror(r)); return false; }
					for (int t = 0; t < 80 && snd_pcm_writei(pcm, silence.data(), (snd_pcm_uframes_t)(silence.size() / openCh)) < 0; t++)
						std::this_thread::sleep_for(std::chrono::milliseconds(50));
					continue;
				}
				giveUp = 0;
				frames -= (size_t)w; p += (size_t)w * openCh;
			}
			return true;
		};
		const Clip* cur = nullptr;
		size_t at = 0;			// cur's next frame
		size_t bgmAt = 0;		// the loop's next frame
		auto lastSound = std::chrono::steady_clock::now();
		for (;;)
		{
			std::string name;
			{
				std::unique_lock<std::mutex> l(m);
				// closed: nothing to do until a cue, a game's release, or the loop wanting the device again
				if (!pcm) cv.wait(l, [this] { return stop || closeNow || screenOff.load() || !queued.empty() || bgmOn.load() || warmUp.load(); });
				if (stop) break;
				if (closeNow) { closeDev("release"); cur = nullptr; closeNow = false; cv.notify_all(); continue; }	// a game is starting
				// the burn-in guard blanked the screen (idle.h): the real idle, so let the device go -- unless a
				// cue came with it, in which case the screen is being used again
				if (screenOff.exchange(false) && queued.empty())
				{
					closeDev("screen off");
					cur = nullptr;
					continue;
				}
				name = queued;
				queued.clear();
			}
			auto now = std::chrono::steady_clock::now();
			// The loop opens the device as soon as the setting turns on (bgmStartAt is set in the constructor and
			// not pushed forward, so it is already past) and keeps it open; a closed device stays open for it
			// instead of closing after a quiet spell. The amp settles while writeFrames retries in place.
			const bool wantBgm = bgmOn.load() && bgmLoaded && (devOpen.load() || now >= bgmStartAt);
			// the boot warm-up opens and holds the device the same way, with no loop to play (the splash's cue)
			const bool wantOpen = wantBgm || warmUp.load();
			if (!name.empty())
			{
				static bool loggedCue = false;
				if (!loggedCue || name == "insert") { slog("cue %s", name.c_str()); loggedCue = true; }
				const Clip& c = clips[name];
				if (pcm && (openRate != c.rate || openCh != c.channels)) closeDev("format");
				if (!pcm && !openDev(c))
				{
					// the card is not up yet (a boot-time cue): keep the cue and try again, so the splash's click
					// plays once it is, instead of being lost to the failed open
					if (warmUp.load())
					{
						std::lock_guard<std::mutex> l(m);
						if (queued.empty()) queued = name;
						std::this_thread::sleep_for(std::chrono::milliseconds(250));
					}
					continue;
				}
				cur = &c;
				at = 0;
				lastSound = now;
				warmUp = false;	// the device is open and the cue is playing: the boot warm-up is done
			}
			else if (!cur)
			{
				// Nothing to play: the device stays open (a cue is then instant). Only the warm-up has a deadline,
				// so a splash with no cue cannot hold it forever, and the guard's blank and a game's release are
				// what else let it go.
				const auto since = std::max(lastSound, warmAt);
				if (warmUp.load() && now - since >= quiet) warmUp = false;	// the cue never came: stop holding the device
				// the loop is off and the menu has been quiet: let the device go too. A stream held open for nothing
				// keeps the Speaker front end busy, and the sound server then drops the Speaker sink (the RPC's games)
				if (pcm && !wantOpen && now - lastSound >= quiet) { closeDev("quiet"); continue; }
				if (!pcm && wantOpen && !openDev(warmUp.load() ? warmClip : bgm))
				{
					if (warmUp.load()) std::this_thread::sleep_for(std::chrono::milliseconds(250));	// the card isn't up yet
					continue;
				}
			}
			if (!pcm) continue;

			const bool haveBgm = wantBgm && bgmLoaded && bgmGain.load() > 0;
			size_t nframes = (size_t)(openRate / 200);	// the next 5 ms: the cue, the loop, or silence
			if (!cur && !haveBgm)
			{
				if (!writeFrames(silence.data(), silence.size() / openCh)) { closeDev("write err (silence)"); continue; }
			}
			else
			{
				// the cue at full level, the ambient loop (a few percent) under it: one stream, mixed here
				if (mix.size() < nframes * openCh) mix.resize(nframes * openCh);
				std::fill(mix.begin(), mix.begin() + nframes * openCh, 0);
				if (cur)
				{
					size_t total = cur->pcm.size() / openCh;
					size_t take = std::min(nframes, total - at);
					const int16_t* s = cur->pcm.data() + at * openCh;
					std::copy(s, s + take * openCh, mix.begin());
					at += take;
					if (at >= total) { cur = nullptr; lastSound = now; }
				}
				if (haveBgm && !bgm.pcm.empty())
				{
					size_t total = bgm.pcm.size() / openCh;
					int g = bgmGain.load();
					for (size_t i = 0; i < nframes; i++)
					{
						size_t j = bgmAt + i; if (j >= total) j -= total;	// (bgmAt < total, nframes short)
						for (int c = 0; c < openCh; c++)
						{
							int v = mix[i * openCh + c] + (int)bgm.pcm[j * openCh + c] * g / 1000;
							if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
							mix[i * openCh + c] = (int16_t)v;
						}
					}
					bgmAt = (bgmAt + nframes) % total;
				}
				if (!writeFrames(mix.data(), nframes)) { closeDev("write err (mix)"); cur = nullptr; continue; }
			}
		}
		closeDev("end");
#else
		std::unique_lock<std::mutex> l(m);
		for (;;)
		{
			cv.wait(l, [this] { return stop || closeNow || !queued.empty(); });
			if (stop) break;
			closeNow = false;
			queued.clear();
		}
#endif
	}
}
