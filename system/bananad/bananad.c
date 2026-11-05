/* bananad: Banana's power daemon for the Retroid Pocket Classic (SM4450).
 *
 *   - Sleep: a short press of the power button, or (in the menu) a few idle minutes put the handheld into
 *     suspend-to-RAM. The running game is paused first (SIGSTOP to its process group) and resumed after, the hooks
 *     in /etc/banana/sleep.d run around it, and banana-ui is told to draw its panel again. Pressing power wakes it
 *     (the device tree marks it a wakeup source).
 *   - A long press of the power button shuts down cleanly.
 *   - Volume keys change the mixer; F (BTN_MODE) + volume changes the backlight.
 *   - A battery about to run out shuts the handheld down before the data partition can be cut off mid-write.
 *   - The charger: bananad watches the power supplies and pulses the vibrator when the cable goes in. The handheld
 *     has no LEDs, so there is nothing else to drive.
 *   - Brightness and volume are kept across reboots (/data/banana/backlight, /data/banana/asound.state).
 *   - Sound is the server's (PipeWire + WirePlumber, brought up by rcS's sound-server): ACP loads the RPC's UCM2
 *     profile and owns the DSP's routing and levels. bananad keeps ALSA's default device pointed at it
 *     (/run/banana/asound.conf, which /etc/asound.conf reads) and changes the amplifier's level for the volume keys.
 *
 * Settings: /etc/banana/bananad.conf, then /data/banana/bananad.conf (key = value lines, see bananad.conf); SIGHUP
 * reads them again (the menu's settings screen sends it).
 * It reads the input devices alongside banana-ui and the games (evdev allows many readers) and grabs nothing.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define MAXDEV 32

/* usleep is obsolete in POSIX.1-2008; nanosleep is its replacement */
static void sleep_us(long us)
{
	struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
	while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

static struct {
	char mixer[64];			/* ALSA simple control the volume keys change; "auto": the playing card's own */
	int volume_step;		/* percent */
	int brightness_step;	/* percent of max */
	int autosleep_min;		/* idle minutes in the menu before sleeping; 0 = never */
	int power_long_ms;		/* holding power this long shuts down */
	int battery_off_pct;	/* at or under this, discharging: shut down */
	char mem_sleep[16];		/* deep (suspend to RAM) or s2idle */
	double sleep_check_hours;	/* asleep: wake this often to look at the battery (0: never) */
	int sleep_poweroff_pct;	/* found at or under this while asleep: shut down cleanly */
	int sleep_floor_pct;		/* discharging at or under this: refuse a sleep when no RTC alarm can wake it */
	double dim_min;			/* idle minutes in the menu before the panel dims (0: never) */
	int dim_pct;			/* dimmed to this percent of their brightness */
	int vibrate_ms;			/* the plug-in pulse length; the motor's drive is already the driver's maximum */
	char sleep_skip[64];		/* debug: comma list of suspend steps to skip (stop, ui, mute, hooks, sync) */
	int autosleep_set;		/* autosleep_minutes was written down somewhere: it is asked for, not defaulted */
} cfg = { "auto", 5, 8, 10, 2000, 3, "deep", 4, 5, 15, 2, 30, 600, "", 0 };

/* a suspend step named in sleep_skip: only ever set by hand (banana/bananad.conf), to bisect a sleep that comes straight
 * back on the handheld without rebuilding bananad */
static int skip(const char *what) { return cfg.sleep_skip[0] && strstr(cfg.sleep_skip, what) != NULL; }

static int devfd[MAXDEV], ndev;
static void power_update(void);
static void audio_route(void);
static void jack_settle_begin(void);
static void jack_settle_poll(void);
static void jack_log(void);
static void ra_cmd(const char *cmd);
static void undim(void);
static void find_panel_bl(void);
static char panel_bl[64];	/* the panel's backlight name; banana-ui's idle dim owns it while it runs */
static int headphones;
static int fn_held, select_held, power_down;
static int panel_blanked;	/* bananad blanked the panel (a suspend it could not do): the next key brings it back */
static double power_since, last_input, ignore_power_until;
static void sleep_quiet_begin(void);	/* the panel dark and the codec muted, before the DSP sleeps */
static void sleep_quiet_end(void);	/* and back, on the way out of it */

static volatile sig_atomic_t reload;
static void on_hup(int sig) { (void)sig; reload = 1; }

static double mono(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsyslog(LOG_INFO, fmt, ap);
	va_end(ap);
}

static void trim(char *s)
{
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
	char *b = s;
	while (*b && isspace((unsigned char)*b)) b++;
	memmove(s, b, strlen(b) + 1);
}

/* load_conf <file> <asked>: read the settings. <asked> is 1 for the file on the data partition -- the one a person
 * or the settings screen wrote -- and 0 for the image's own defaults, which are only the starting point. It decides
 * which numbers count as a wish (see the idle rule in main()). */
static void load_conf(const char *path, int asked)
{
	FILE *f = fopen(path, "r");
	if (!f) return;
	char ln[256];
	while (fgets(ln, sizeof ln, f)) {
		char *eq = strchr(ln, '=');
		if (ln[0] == '#' || !eq) continue;
		*eq = 0;
		char *k = ln, *v = eq + 1;
		trim(k); trim(v);
		if (!strcmp(k, "mixer")) snprintf(cfg.mixer, sizeof cfg.mixer, "%s", v);
		else if (!strcmp(k, "volume_step")) cfg.volume_step = atoi(v);
		else if (!strcmp(k, "brightness_step")) cfg.brightness_step = atoi(v);
		else if (!strcmp(k, "autosleep_minutes")) { cfg.autosleep_min = atoi(v); if (asked) cfg.autosleep_set = 1; }
		else if (!strcmp(k, "power_long_ms")) cfg.power_long_ms = atoi(v);
		else if (!strcmp(k, "battery_shutdown_percent")) cfg.battery_off_pct = atoi(v);
		else if (!strcmp(k, "mem_sleep")) snprintf(cfg.mem_sleep, sizeof cfg.mem_sleep, "%s", v);
		else if (!strcmp(k, "sleep_check_hours")) cfg.sleep_check_hours = atof(v);
		else if (!strcmp(k, "sleep_poweroff_percent")) cfg.sleep_poweroff_pct = atoi(v);
		else if (!strcmp(k, "sleep_floor_percent")) cfg.sleep_floor_pct = atoi(v);
		else if (!strcmp(k, "dim_minutes")) cfg.dim_min = atof(v);
		else if (!strcmp(k, "dim_percent")) cfg.dim_pct = atoi(v);
		else if (!strcmp(k, "vibrate_ms")) cfg.vibrate_ms = atoi(v);
		else if (!strcmp(k, "sleep_skip")) snprintf(cfg.sleep_skip, sizeof cfg.sleep_skip, "%s", v);
	}
	fclose(f);
}

/* the time zone the settings screen keeps in banana-ui's settings.conf (a POSIX rule), for sleep.log's times */
static void load_timezone(void)
{
	FILE *f = fopen("/data/banana/settings.conf", "r");
	if (!f) return;
	char ln[256];
	while (fgets(ln, sizeof ln, f)) {
		char *eq = strchr(ln, '=');
		if (ln[0] == '#' || !eq) continue;
		*eq = 0;
		char *k = ln, *v = eq + 1;
		trim(k); trim(v);
		if (!strcmp(k, "timezone") && *v) { setenv("TZ", v, 1); tzset(); }
	}
	fclose(f);
}

static int write_file(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);	/* as a shell's > does (sysfs takes it) */
	if (fd < 0) return -1;
	ssize_t n = write(fd, s, strlen(s));
	int e = errno;
	close(fd);
	errno = e;
	return n == (ssize_t)strlen(s) ? 0 : -1;
}

static int read_int(const char *path, int def)
{
	FILE *f = fopen(path, "r");
	if (!f) return def;
	int v = def;
	if (fscanf(f, "%d", &v) != 1) v = def;
	fclose(f);
	return v;
}

static void read_str(const char *path, char *out, size_t n)
{
	out[0] = 0;
	FILE *f = fopen(path, "r");
	if (!f) return;
	if (fgets(out, (int)n, f)) trim(out);
	fclose(f);
}

/* fork + exec, no shell; waits */
static int run(char *const argv[])
{
	pid_t p = fork();
	if (p == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	if (p < 0) return -1;
	int st = 0;
	while (waitpid(p, &st, 0) < 0)
		if (errno != EINTR) return -1;
	if (WIFEXITED(st)) return WEXITSTATUS(st);
	if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
	return -1;
}

/* every executable in /etc/banana/sleep.d, in name order, with "pre" or "post"; background: not waited for (after a
 * wake, the game and the menu come first and Wi-Fi catches up) */
static void hooks_run(const char *when);
static void hooks(const char *when, int background)
{
	if (!background) { hooks_run(when); return; }
	pid_t p = fork();
	if (p == 0) {
		if (fork() == 0) { setsid(); hooks_run(when); _exit(0); }
		_exit(0);
	}
	if (p > 0) while (waitpid(p, NULL, 0) < 0 && errno == EINTR) {}
}

static void hooks_run(const char *when)
{
	struct dirent **list;
	int n = scandir("/etc/banana/sleep.d", &list, NULL, alphasort);
	if (n < 0) return;
	for (int i = 0; i < n; i++) {
		char path[300];
		snprintf(path, sizeof path, "/etc/banana/sleep.d/%s", list[i]->d_name);
		struct stat st;
		if (list[i]->d_name[0] != '.' && !stat(path, &st) && S_ISREG(st.st_mode) && (st.st_mode & 0111)) {
			char *argv[] = { path, (char *)when, NULL };
			run(argv);
		}
		free(list[i]);
	}
	free(list);
}

/* The pid file names a process, but a pid is reused: after a launcher is killed (or a game SIGKILLed) its file stays
 * with a pid that, later in the same boot, can belong to someone else -- and suspend SIGSTOPs that process's whole
 * group, shutdown SIGTERMs it and a wake SIGUSR1s it. So the file only counts when the process did not start *after*
 * the file was written: a reused pid always did. The process's age comes from /proc/uptime (monotonic); the file's is
 * a wall-clock difference, which a step of the clock (ntpd at Wi-Fi, hwclock) inflates -- and an inflated age reads
 * as a reused pid, so a running game would look gone. A pidfile cannot be older than the boot, so a file age beyond
 * the uptime means the clock moved, not the file: it is read as "written at boot", which still refuses a process
 * that began well after it. (Reading it as zero would let a stale file whose pid was reused pass for the whole
 * boot.) The 3 s slack covers the write/exec gap and the file's coarse timestamp. */
static unsigned long long proc_start_ticks(int pid)
{
	char path[64], buf[512];
	snprintf(path, sizeof path, "/proc/%d/stat", pid);
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	unsigned long long start = 0;
	if (fgets(buf, sizeof buf, f)) {
		char *rp = strrchr(buf, ')');	/* the command name can hold spaces and parentheses */
		/* after ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime cutime cstime
		 * priority nice threads itrealvalue starttime (field 22) */
		if (rp && sscanf(rp + 2, "%*s %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu", &start) != 1)
			start = 0;
	}
	fclose(f);
	return start;
}

static pid_t pid_of(const char *pidfile)
{
	int p = read_int(pidfile, 0);
	if (p <= 0 || kill(p, 0) != 0) return 0;
	struct stat st;
	unsigned long long start = proc_start_ticks(p);
	long hz = sysconf(_SC_CLK_TCK);
	if (start && hz > 0 && !stat(pidfile, &st)) {
		double up = 0;
		char b[32];
		read_str("/proc/uptime", b, sizeof b);
		if (sscanf(b, "%lf", &up) != 1) up = 0;
		double age = up - (double)start / (double)hz;	/* how long the process has run, seconds */
		double file = (double)time(NULL) - (double)st.st_mtime;	/* how long ago the file was written */
		if (up > 0 && file > up + 5.0) file = up;	/* older than the boot: the clock stepped, so it says nothing */
		if (up > 0 && age < file - 3.0) return 0;	/* it began after the file named it: a reused pid */
	}
	return p;
}

/* banana-ui's pid, by its name in /proc. Cached: the scan ran on every wake and on every volume/brightness key's
 * OSD; it is repeated only when the cached pid has gone (banana-ui restarted) */
static pid_t ui_pid_cache;
static pid_t ui_pid(void)
{
	if (ui_pid_cache > 0 && kill(ui_pid_cache, 0) == 0) return ui_pid_cache;
	DIR *d = opendir("/proc");
	if (!d) return ui_pid_cache;
	struct dirent *e;
	pid_t r = 0;
	while (!r && (e = readdir(d))) {
		if (!isdigit((unsigned char)e->d_name[0])) continue;
		char path[300], comm[32];
		snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
		read_str(path, comm, sizeof comm);
		if (!strcmp(comm, "banana-ui")) r = atoi(e->d_name);
	}
	closedir(d);
	if (r > 0) ui_pid_cache = r;
	return r;
}

/* the menu's level card for a volume or brightness key: what changed and its percent in /run/banana/osd, then
 * SIGUSR2 tells banana-ui to show it (it ignores a file older than a moment, e.g. one written during a game) */
static void osd(const char *what, int percent)
{
	char v[64];
	snprintf(v, sizeof v, "%s %d\n", what, percent);
	FILE *f = fopen("/run/banana/osd.new", "w");
	if (!f) return;
	fputs(v, f);
	fclose(f);
	rename("/run/banana/osd.new", "/run/banana/osd");
	pid_t ui = ui_pid();
	if (ui > 0) kill(ui, SIGUSR2);
}

/* the headphone jack's state as the kernel has it (the codec's jack is an input device with a switch) */
static int headphones_in(void)
{
	for (int i = 0; i < ndev; i++) {
		unsigned long sw[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = { 0 };
		if (ioctl(devfd[i], EVIOCGSW(sizeof sw), sw) >= 0 &&
		    (sw[SW_HEADPHONE_INSERT / (8 * sizeof(long))] >> (SW_HEADPHONE_INSERT % (8 * sizeof(long))) & 1))
			return 1;
	}
	return 0;
}

/* The direct look at the jack (the still-to-do check in the port log): the input device that carries SW_HEADPHONE_INSERT,
 * its name and the state read now -- the same read headphones_in() acts on. The WCD937x MBHC (which backs that device)
 * is known to answer before it has measured the socket, so on a boot that comes up silent this says whether the codec
 * claimed "headphones in" with nothing plugged. */
static void jack_log(void)
{
	static int said_absent, said_present;
	size_t w = SW_HEADPHONE_INSERT / (8 * sizeof(long)), b = SW_HEADPHONE_INSERT % (8 * sizeof(long));
	int found = 0;
	for (int i = 0; i < ndev; i++) {
		unsigned long cap[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = { 0 };
		if (ioctl(devfd[i], EVIOCGBIT(EV_SW, sizeof cap), cap) < 0 || !((cap[w] >> b) & 1)) continue;
		unsigned long st[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = { 0 };
		int in = ioctl(devfd[i], EVIOCGSW(sizeof st), st) >= 0 && ((st[w] >> b) & 1);
		char name[64] = "?";
		ioctl(devfd[i], EVIOCGNAME(sizeof name), name);
		if (!said_present) say("sound: the jack switch on '%s' reads %s", name, in ? "in" : "out");
		found = 1;
	}
	if (found) said_present = 1;
	/* The jack is an input device that probes with the codec, and bananad can open the inputs before it appears: with
	 * no such device, headphones_in() answers "nothing in" by absence, not by measurement. Say which it is. */
	else if (!said_absent) { said_absent = 1; say("sound: no input device carries SW_HEADPHONE_INSERT yet"); }
}

static void drain_inputs(void)
{
	struct input_event ev[64];
	for (int i = 0; i < ndev; i++)
		while (read(devfd[i], ev, sizeof ev) > 0) {}
}

/* Did the user really press power while it slept? The PMIC pwrkey is the only armed wake source and its IRQ can fire
 * on a glitch (and on the release of the very press that asked for the sleep), so a wake only counts when a real
 * KEY_POWER press is among the queued events. The events are consumed here, so the wake-up sequence's drain_inputs()
 * then finds nothing. */
static int wake_key_press(void)
{
	int pressed = 0;
	struct input_event ev[64];
	for (int i = 0; i < ndev; i++) {
		ssize_t r;
		while ((r = read(devfd[i], ev, sizeof ev)) > 0)
			for (ssize_t k = 0; k < r / (ssize_t)sizeof ev[0]; k++)
				if (ev[k].type == EV_KEY && ev[k].code == KEY_POWER && ev[k].value == 1) pressed = 1;
	}
	return pressed;
}

/* ---- sleep ------------------------------------------------------------------------------------------------------
 * Pressing power (or an idle timer): the game is paused, the pre-sleep hooks run (Wi-Fi down, 10-wifi), the card is
 * flushed (the kernel syncs too: this config does not set SUSPEND_SKIP_SYNC, so it is a belt-and-braces), an RTC alarm
 * is set for the battery check, and the SoC suspends to RAM. On waking,
 * the game and the menu go on first, then the post-sleep hooks in the background. Woken by the battery check:
 * straight back to sleep, without running anything again. */
static void shutdown_now(const char *why);
static int battery_discharging_pct(void);
static long battery_charge_uah(void);
static int sleep_fallback;		/* 0 as configured; 1 devices one at a time; 2 also s2idle (a suspend failed) */

static void sleep_log(const char *fmt, ...)
{
	char line[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	say("%s", line);
	const char *path = "/data/banana/logs/sleep.log";
	struct stat st;
	if (!stat(path, &st) && st.st_size > 64 * 1024) rename(path, "/data/banana/logs/sleep.log.1");
	FILE *f = fopen(path, "a");
	if (!f) return;
	time_t now = time(NULL);
	char when[32];
	strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
	fprintf(f, "%s %s\n", when, line);
	fclose(f);
}

/* the RTC alarm that wakes the handheld for the battery check; 0 clears it. Returns the alarm's time (0: none) */
static time_t rtc_alarm(int seconds)
{
	write_file("/sys/class/rtc/rtc0/wakealarm", "0");
	if (seconds <= 0) return 0;
	char v[32];
	snprintf(v, sizeof v, "+%d", seconds);
	if (write_file("/sys/class/rtc/rtc0/wakealarm", v)) return 0;
	return time(NULL) + seconds;
}

/* The RPC's PMIC RTC has no alarm (`qcom,no-alarm`: the ADSP owns its interrupt), so wakealarm cannot be written and the
 * sleep_check_hours battery check -- and with it sleep_poweroff_percent -- can never fire. Say which at start: a
 * sleeping handheld then comes back only on the power key or the charger, so it must not be left asleep unattended or
 * the battery drains flat. */
static int rtc_wake_possible(void)
{
	return access("/sys/class/rtc/rtc0/wakealarm", W_OK) == 0;
}

/* Refuse to deep-sleep a nearly empty battery when nothing can wake it on a timer. With the RPC's ADSP-owned RTC alarm
 * there is no wakealarm, so a sleeping low battery would drain flat and never come back; instead the caller leaves the
 * panel dark and keeps the SoC awake, so the 60 s poll can still shut down cleanly at battery_shutdown_percent. */
static int sleep_too_low(void)
{
	if (rtc_wake_possible()) return 0;	/* a real alarm: the battery check wakes it in time */
	int pct = battery_discharging_pct();
	return pct >= 0 && pct <= cfg.sleep_floor_pct;
}

/* Why the kernel resumed. It cannot say on this board: /sys/power/pm_wakeup_irq is absent, and a real power-key wake
 * leaves every wakeup_sources counter at 0 (the PMIC's PON IRQ is not a tracked wakeup source; mmc0, which the table's
 * last_change used to name for every wake, never fires at all -- measured with a key wake on the handheld: pwrkey 0/0).
 * So report what bananad itself saw: the power key press (wake_key_press()), the cable, or neither -- plus the kernel's
 * global wakeup_count, which says whether it counted a wakeup event at all. */
static void wake_reason(char *out, size_t n, int key, int cable, unsigned long wk0)
{
	unsigned long wk = 0;
	FILE *f = fopen("/sys/power/wakeup_count", "r");
	if (f) { if (fscanf(f, "%lu", &wk) != 1) wk = 0; fclose(f); }
	const char *seen = key ? "power key" : cable ? "cable" : "no key and no cable";
	if (wk > wk0) snprintf(out, n, "wake: %s (kernel wakeup_count +%lu)", seen, wk - wk0);
	else snprintf(out, n, "wake: %s (the kernel counted no wakeup event)", seen);
}

/* The menu's handover, waited for rather than guessed at. After SIGHUP, banana-ui releases the DSP and closes its PCM
 * (Sound::release waits up to 2 s for the sound thread), publishing each step to /run/banana/sound-state. A PCM close
 * still in flight when the DSP is told to sleep is what the overnight hang on 03:40 might have been (logs/sleep-state
 * from the next sleep said "releasing (handover)" at the write of mem). So: 1 = it closed (ms), 0 = still open after
 * the menu's own 2 s, -1 = no note at all (a menu from before this, which gets the old fixed 200 ms). */
static int ui_release_wait(double *ms)
{
	double t = mono();
	for (;;) {
		char s[128] = "";
		read_str("/run/banana/sound-state", s, sizeof s);
		if (!s[0]) return -1;
		if (!strstr(s, "open") && !strstr(s, "releasing")) { *ms = (mono() - t) * 1000; return 1; }
		if (mono() - t > 2.0) { *ms = (mono() - t) * 1000; return 0; }
		sleep_us(20000);
	}
}

/* What the handheld looked like as the kernel went under, on the data partition. A sleep whose resume never returns
 * leaves no "awake" line and no post hook, so this note (overwritten every sleep) is the only thing the next boot can
 * read: the mode, and whether the menu still had its PCM open -- a close in flight when the DSP was told to sleep is
 * the one thing the hang at 03:40 on the handheld could have been doing (logs/banana-ui.log.1: the device was
 * reopened 16 s before that sleep and handed over 200 ms before the write of "mem"). */
static void sleep_note(const char *why, const char *mode, int sources)
{
	char path[] = "/data/banana/logs/sleep-state";
	struct stat st;
	if (!stat(path, &st) && st.st_size > 32 * 1024) rename(path, "/data/banana/logs/sleep-state.1");
	FILE *f = fopen(path, "w");
	if (!f) return;
	time_t now = time(NULL);
	char when[32], sound[128] = "";
	strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
	read_str("/run/banana/sound-state", sound, sizeof sound);
	fprintf(f, "%s going to sleep (%s)\n", when, why);
	fprintf(f, "mode: %s, wakeup sources in the table: %d\n", mode, sources);
	fprintf(f, "menu: %d, sound device: %s\n", (int)ui_pid(), sound[0] ? sound : "(no note)");
	fprintf(f, "game: %d\n", (int)pid_of("/run/banana/game.pid"));
	fclose(f);
}

/* and the kernel's own record of the trip, on the way out (the note above is the only survivor of one that did not) */
static void sleep_note_resume(const char *what)
{
	FILE *f = fopen("/data/banana/logs/sleep-state", "a");
	if (!f) return;
	time_t now = time(NULL);
	char when[32];
	strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
	fprintf(f, "%s awake: %s\n", when, what);
	FILE *p = popen("dmesg 2>/dev/null | grep -E 'PM: (suspend|resume)' | tail -n 6", "r");
	if (p) {
		char ln[160];
		while (fgets(ln, sizeof ln, p)) fputs(ln, f);
		pclose(p);
	}
	fclose(f);
}

/* Only the owner may wake the handheld. The kernel arms every device that says it can wake -- the rsinput MCU's
 * serial (988000.serial), the Wi-Fi/ADSP remoteprocs (8a00000/3000000), gpio-keys (the volume keys), the thermal
 * sensors, mmc0 -- and any of them raising an interrupt during a sleep is a resume nobody asked for (the serial MCU
 * and the Wi-Fi link were the ones seen waking it). So just before suspending, leave the PMIC pwrkey armed, keep the
 * charger/USB supplies armed (the cable is a wake the owner did ask for -- it is the one press that has to work when
 * the pwrkey line glitches, which is exactly what it does: see the sleep.log's "wake with no key or cable"), and
 * disable the rest. The drivers re-arm their own wakeup on resume, which is why this runs before every sleep rather
 * than once at boot. mmc0 is a virtual source with no power/wakeup file to write, so it cannot be disarmed here;
 * logs/sleep-state records how many sources there are, and a wake now names the source whose count actually moved. */
static void wake_only_power(void)
{
	DIR *d = opendir("/sys/class/wakeup");
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "wakeup", 6) != 0) continue;
		char name[160] = "", p[PATH_MAX + 32], dev[PATH_MAX];
		snprintf(p, sizeof p, "/sys/class/wakeup/%s/name", e->d_name);
		read_str(p, name, sizeof name);
		snprintf(p, sizeof p, "/sys/class/wakeup/%s/device", e->d_name);
		if (!realpath(p, dev)) continue;	/* a virtual source (mmc0) has no power/wakeup to write */
		snprintf(p, sizeof p, "%s/power/wakeup", dev);
		int keep = strstr(name, "pwrkey") || strstr(name, "battmgr") || strstr(name, "ucsi");
		write_file(p, keep ? "enabled" : "disabled");
	}
	closedir(d);
}

static void sleep_mode(void)
{
	/* deep: suspend to RAM (TF-A turns the SoC off, DRAM in self-refresh); s2idle as the fallback */
	char modes[128];
	read_str("/sys/power/mem_sleep", modes, sizeof modes);
	const char *want = sleep_fallback >= 2 ? "s2idle" : cfg.mem_sleep;
	char chosen[32];
	snprintf(chosen, sizeof chosen, "[%s]", want);
	if (strstr(modes, want) && !strstr(modes, chosen)) write_file("/sys/power/mem_sleep", want);	/* only if it changes */
	write_file("/sys/power/pm_async", sleep_fallback == 0 ? "1" : "0");
}

/* the backlights off and on again without changing their level */
/* The backlight class's entries, and one of their files. Six callers below each had their own walk of this directory,
 * which is how they drifted apart (one wrote the panel another owned). Names are short ("backlight0", "panel"): a
 * longer one is not a backlight this image has, so it is skipped rather than truncated. */
#define MAX_BL 8
static int backlights(char names[][64], int max)
{
	DIR *d = opendir("/sys/class/backlight");
	if (!d) return 0;
	struct dirent *e;
	int n = 0;
	while (n < max && (e = readdir(d))) {
		if (e->d_name[0] == '.' || strlen(e->d_name) >= 64) continue;
		snprintf(names[n++], 64, "%s", e->d_name);
	}
	closedir(d);
	return n;
}

static void bl_file(char *out, size_t n, const char *name, const char *what)
{
	snprintf(out, n, "/sys/class/backlight/%.63s/%.31s", name, what);	/* the bounds keep -Wformat-truncation quiet */
}

static void backlights_power(int on)
{
	char bl[MAX_BL][64];
	int n = backlights(bl, MAX_BL);
	for (int i = 0; i < n; i++) {
		char p[320];
		bl_file(p, sizeof p, bl[i], "bl_power");
		write_file(p, on ? "0" : "4");
	}
}

static int charger_online(void);
/* Mass storage read-write "transfer mode" (usb-msc, Settings > System > Mass storage): the card is the computer's
 * disk and /data is unmounted here. Do not sleep the handheld then -- a suspend would stall the transfer mid-write. */
static int msc_rw(void)
{
	char m[8];
	read_str("/run/banana/usb-msc.mode", m, sizeof m);
	return !strcmp(m, "rw");
}
static void suspend_now(const char *why)
{
	double t0 = mono();
	if (msc_rw()) {
		sleep_log("not sleeping (%s): USB transfer mode (the card is the computer's disk)", why);
		return;
	}
	if (sleep_too_low()) {
		/* Nothing can wake it on a timer: a deep sleep here is a handheld that only comes back if someone presses
		 * power. Leave the panel dark (what a sleep looks like) and stay awake, so the 60 s battery poll can still
		 * shut down cleanly. The next key press brings the panel back (see panel_blanked in handle()). */
		sleep_log("not sleeping (%s): %d%% and no RTC wake alarm: dark, but watching the battery", why,
		    battery_discharging_pct());
		backlights_power(0);
		panel_blanked = 1;
		last_input = mono();
		return;
	}
	pid_t game = pid_of("/run/banana/game.pid");
	pid_t pg = game ? getpgid(game) : 0;
	if (pg > 0 && !skip("stop")) kill(-pg, SIGSTOP);	/* an emulator's dynarec mustn't run on while the clocks drop */
	/* the menu's ambient loop, if any: stop it and have it release the DSP before the ADSP sleeps (SIGHUP). Wait for
	 * the close it publishes to /run/banana/sound-state instead of guessing at a delay -- a PCM close still in flight
	 * when the DSP goes under is the one thing the overnight hang of 03:40 was seen doing (logs/sleep-state says
	 * "releasing (handover)" at the write of mem); then darken the panel and mute the codec so nothing flashes or pops */
	if (!game && !skip("ui")) {
		pid_t u = ui_pid();
		if (u > 0) {
			kill(u, SIGHUP);
			double ms = 0;
			int rel = ui_release_wait(&ms);
			if (rel == 1) sleep_log("menu: released the sound device in %.0f ms", ms);
			else if (rel == 0) sleep_log("menu: the sound device was still open %.0f ms after the handover: sleeping anyway", ms);
			else sleep_us(200000);	/* a menu from before sound-state: the old fixed wait */
		}
	}
	if (!skip("mute")) sleep_quiet_begin();
	if (!skip("hooks")) hooks("pre", 0);
	if (!skip("sync")) sync();
	sleep_mode();
	wake_only_power();	/* the power key (and the cable) may bring it back (see its comment) */
	double t_prep = mono();
	if (cfg.sleep_skip[0]) sleep_log("sleep %s: skipping '%s' (debug)", why, cfg.sleep_skip);
	int cycles = 0, rtc_wakes = 0, failures = 0, spurious = 0;
	int plugged0 = charger_online();
	int pct0 = battery_discharging_pct();	/* -1 on the charger: a draining sleep is the one worth measuring */
	long q0 = battery_charge_uah();
	double asleep = 0;
	const char *woke = "?";
	int woke_key = 0, woke_cable = 0;
	unsigned long wk0 = 0;
	for (;;) {
		time_t alarm = rtc_alarm((int)(cfg.sleep_check_hours * 3600));
		double t_in = mono();
		struct timespec b0, b1;
		clock_gettime(CLOCK_BOOTTIME, &b0);	/* BOOTTIME counts the time asleep, MONOTONIC doesn't */
		if (cycles == 0) sleep_log("sleep (%s): prep %.0f ms, %.0f ms to suspend%s", why,
		    (t_prep - t0) * 1000, (t_in - t0) * 1000, game ? ", game paused" : "");
		char mode[32] = "";
		read_str("/sys/power/mem_sleep", mode, sizeof mode);
		{	/* how many wakeup sources the kernel still has armed, for the note (3 with the power key and the cable) */
			FILE *wf = fopen("/sys/kernel/debug/wakeup_sources", "r");
			int n = 0;
			char ln[256];
			if (wf) { while (fgets(ln, sizeof ln, wf)) n++; fclose(wf); }
			sleep_note(why, mode[0] ? mode : cfg.mem_sleep, n > 0 ? n - 1 : 0);
		}
		wk0 = 0;
		{
			FILE *f = fopen("/sys/power/wakeup_count", "r");
			if (f) { if (fscanf(f, "%lu", &wk0) != 1) wk0 = 0; fclose(f); }
		}
		int r = write_file("/sys/power/state", "mem");
		int err = errno;
		/* a signal (SIGHUP from the menu, SIGCHLD) interrupted the write: nothing slept, so retry rather than
		 * counting it as a failed suspend and falling back to s2idle */
		if (r && err == EINTR) { rtc_alarm(0); sleep_us(50000); continue; }
		clock_gettime(CLOCK_BOOTTIME, &b1);
		/* the RTC clears wakealarm when the alarm fires: set before, empty now, is the battery check's wake */
		char pending[32];
		read_str("/sys/class/rtc/rtc0/wakealarm", pending, sizeof pending);
		int rtc_fired = alarm && (!pending[0] || time(NULL) >= alarm);
		rtc_alarm(0);
		cycles++;
		if (r) {
			/* it didn't suspend: devices one at a time, then the lighter s2idle, then give up and blank the panel */
			failures++;
			sleep_log("suspend failed (%s)%s", strerror(err), sleep_fallback < 2 ? ": trying the safer way" : "");
			if (sleep_fallback < 2 && failures <= 2) { sleep_fallback++; sleep_mode(); sleep_us(200000); continue; }
			woke = "failed";
			/* It never slept: leave the panel dark -- that is what a sleep would have looked like, and the
			 * next press brings it back (see panel_blanked in handle()) -- and let the game run again, because
			 * a frozen emulator behind a dark screen is worse. Nothing here is a wake, so the wake bookkeeping
			 * below is skipped. */
			if (pg > 0) kill(-pg, SIGCONT);
			sleep_quiet_end();
			backlights_power(0);
			/* the pre hooks have run (10-wifi took Wi-Fi down): run the post ones too, or a failed suspend would
			 * leave the handheld with no network until the next sleep succeeds */
			hooks("post", 1);
			panel_blanked = 1;
			sleep_log("could not suspend after %d tries: the panel is dark until the next press", failures);
			last_input = mono();
			return;
		}
		asleep += (b1.tv_sec - b0.tv_sec) + (b1.tv_nsec - b0.tv_nsec) / 1e9;
		/* why it woke: the battery check's alarm, or the power button */
		if (rtc_fired) {
			rtc_wakes++;
			int pct = battery_discharging_pct();
			if (pct >= 0 && pct <= cfg.sleep_poweroff_pct) {
				sleep_log("battery check: %d%% after %.1f h asleep: shutting down", pct, asleep / 3600);
				if (pg > 0) kill(-pg, SIGCONT);	/* the game gets SIGTERM next, to write its saves */
				shutdown_now("battery empty while asleep");
				return;
			}
			continue;	/* enough battery: back to sleep at once */
		}
		/* The pwrkey is the only armed wake source and its IRQ is glitchy (it fires on the release of the very press
		 * that asked for this sleep), so a wake with no real KEY_POWER press and no cable change is the panel lighting
		 * up for nothing: go back to sleep, a bounded number of times, and only then stay awake. */
		woke_key = wake_key_press();
		int cable = charger_online();
		woke_cable = cable && !plugged0;
		/* the cable going the other way is the charger's edge, not a phantom: nothing to stay awake for, and it is
		 * not one of the three tries a real phantom gets */
		if (plugged0 && !cable) {
			sleep_log("the charger was unplugged: back to sleep");
			continue;
		}
		if (spurious < 3 && !woke_key && !woke_cable) {
			spurious++;
			sleep_log("wake with no key or cable (spurious): back to sleep (%d/3)", spurious);
			continue;
		}
		woke = woke_key ? "power button" : "charger";
		break;
	}
	/* awake: the game and the menu first. A fallback (devices one at a time, or s2idle) was for this sleep: the
	 * next one starts from the configured mode again instead of staying in the safer, higher-power one all session */
	sleep_fallback = 0;
	double t_wake = mono();
	backlights_power(1);
	sleep_quiet_end();
	if (pg > 0) kill(-pg, SIGCONT);
	/* The compositor's output too: a panel its driver disabled on the way down can come back dark, with cage's
	 * frames going nowhere, and only a modeset clears it. cage has no session manager to hear that the system
	 * slept (Banana's seat is deliberately not VT-bound), so it re-modesets on SIGUSR1 -- the pid run-retroarch
	 * wrote when it started it. */
	{
		pid_t cg = pid_of("/run/banana/cage.pid");
		if (cg > 0) kill(cg, SIGUSR1);
	}
	pid_t ui = ui_pid();
	if (ui && !game) kill(ui, SIGUSR1);	/* draws the panel again (the clock and the battery moved on) */
	double t_back = mono();
	undim();
	hooks("post", 1);
	/* the press that woke us comes in now: it is not a new request */
	drain_inputs();
	power_update();
	jack_settle_begin();	/* the codec can answer wrong right after a wake: confirm it in the loop, without blocking */
	ignore_power_until = mono() + 1.0;
	power_down = 0;
	last_input = mono();
	char wr[256], pw[96] = "";
	wake_reason(wr, sizeof wr, woke_key, woke_cable, wk0);
	/* what the sleep cost: the gauge's own percentage either side. %/h only makes sense unplugged and over a real
	 * stretch of sleep -- this is the number an "Android-like" sleep is judged by. */
	int pct1 = battery_discharging_pct();
	long q1 = battery_charge_uah();
	if (pct0 >= 0 && pct1 >= 0)
	{
		if (asleep >= 180 && q0 > 0 && q1 > 0)
			snprintf(pw, sizeof pw, ", battery %d%% -> %d%% (%.0f mAh in %.1f h, %.0f mA%s)", pct0, pct1,
				(q0 - q1) / 1000.0, asleep / 3600.0, (q0 - q1) / 1000.0 / (asleep / 3600.0),
				spurious ? "" : ", no stray wake");
		else
			snprintf(pw, sizeof pw, ", battery %d%% -> %d%%", pct0, pct1);
	}
	sleep_log("awake (%s) after %.0f s asleep%s%s, %s, game and menu back %.1f ms after the kernel%s", woke, asleep,
		rtc_wakes ? ", battery checked" : "", asleep < 5.0 ? " (it did not take)" : "", wr, (t_back - t_wake) * 1000, pw);
	sleep_note_resume(wr);
}

static void shutdown_now(const char *why)
{
	say("shutting down (%s)", why);
	/* The menu's game-exit path must not clear the marker written below: the game leaving is this shutdown, not a
	 * quit to the menu. A file, because the menu cannot see this process's state. */
	FILE *sd = fopen("/run/banana/shutting-down", "w");
	if (sd) fclose(sd);
	/* A game a shutdown interrupts is the one to resume at the next boot: the menu wrote its path when it launched
	 * it, and this has to be read before the game is stopped. Coming from the menu there is nothing to remember, and
	 * a marker left by an older boot goes. */
	char rom[PATH_MAX] = "";
	read_str("/run/banana/game.path", rom, sizeof rom);
	pid_t game = pid_of("/run/banana/game.pid");
	if (game && rom[0]) {
		FILE *f = fopen("/data/banana/resume", "w");
		if (f) { fprintf(f, "%s\n", rom); fclose(f); }
	} else unlink("/data/banana/resume");
	if (game) {
		pid_t pg = getpgid(game);
		kill(pg > 0 ? -pg : game, SIGTERM);	/* let the emulator write its saves */
		for (int i = 0; i < 50 && kill(game, 0) == 0; i++) sleep_us(100000);
	}
	sync();
	/* busybox init runs /etc/init.d/rcK (unmount, sync) on SIGUSR2 = poweroff */
	if (kill(1, SIGUSR2)) reboot(RB_POWER_OFF);
}

static double save_volume_at, save_backlight_at;	/* debounced: written a moment after the last key */

/* ---- sound: the card that plays ----------------------------------------------------------------------------------- */
static int card_play = -1;	/* the card sound plays on; -1: none yet (or none at all) */
static int card_ar = -1;	/* the audioreach card (the RPC): the sound server routes its DSP graph from the UCM2 profile */

/* the card, from /proc/asound/cards (" 0 [sm8450 ]: simple-card - ...") */
/* The card and its route through the ALSA C mixer: no `amixer` fork. The DSP rebuilds its controls with each
 * frontend graph, so every look-up opens and loads a fresh mixer instead of holding a handle whose elements may
 * point at controls that no longer exist. */
static int mixer_open(int card, snd_mixer_t **mx)
{
	char dev[16];
	snprintf(dev, sizeof dev, "hw:%d", card);
	if (snd_mixer_open(mx, 0) < 0) return -1;
	if (snd_mixer_attach(*mx, dev) < 0 || snd_mixer_selem_register(*mx, NULL, NULL) < 0 || snd_mixer_load(*mx) < 0) {
		snd_mixer_close(*mx);
		*mx = NULL;
		return -1;
	}
	return 0;
}

static void audio_find_cards(void)
{
	FILE *f = fopen("/proc/asound/cards", "r");
	if (!f) return;
	char ln[256];
	while (fgets(ln, sizeof ln, f)) {
		int n;
		if (sscanf(ln, " %d [", &n) != 1) continue;
		card_play = n;
		/* the RPC's card: audioreach, with the frontends (MultiMedia1/2) mixed into the speaker I2S and the codec
		 * by mixer controls whose names say so -- what alsa-ucm-conf's profile would set. Nothing does it on this
		 * image, so without this the DSP says "no backend DAIs enabled" and plays nothing. */
		snd_mixer_t *mx = NULL;
		if (mixer_open(n, &mx) == 0) {
			for (snd_mixer_elem_t *e = snd_mixer_first_elem(mx); e; e = snd_mixer_elem_next(e))
				if (snd_mixer_elem_get_type(e) == SND_MIXER_ELEM_SIMPLE &&
				    strstr(snd_mixer_selem_get_name(e), "SENARY_MI2S_RX Audio Mixer")) { card_ar = n; break; }
			snd_mixer_close(mx);
		}
		break;	/* the first card is the one sound plays on */
	}
	fclose(f);
}

/* Named CTL access (what `amixer cset/sset` did), through the ALSA C API: works for DAPM muxes and enums too, not
 * only the simple-mixer elements. */
static int ctl_open(int card, snd_ctl_t **ctl)
{
	char dev[16];
	snprintf(dev, sizeof dev, "hw:%d", card);
	return snd_ctl_open(ctl, dev, 0);
}

static int ctl_set(int card, const char *name, const char *val)
{
	snd_ctl_t *ctl = NULL;
	snd_ctl_elem_id_t *id = NULL;
	snd_ctl_elem_info_t *info = NULL;
	snd_ctl_elem_value_t *v = NULL;
	int r = -1;
	if (ctl_open(card, &ctl) < 0) return -1;
	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, name);
	snd_ctl_elem_id_set_index(id, 0);
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_info_set_id(info, id);
	if (snd_ctl_elem_info(ctl, info) < 0) {
		/* A simple-mixer switch is "<name> Switch" in the raw CTL space ("HPHL" is "HPHL Switch" here); amixer's
		 * sget/sset resolve that short name, the CTL API does not. Try the suffixed name before giving up, so
		 * bananad's late-card fallback (below) can actually set the codec's switches. */
		char alt[192];
		snprintf(alt, sizeof alt, "%s Switch", name);
		snd_ctl_elem_id_set_name(id, alt);
		snd_ctl_elem_info_set_id(info, id);
		if (snd_ctl_elem_info(ctl, info) < 0) goto out;
	}
	unsigned count = snd_ctl_elem_info_get_count(info);
	snd_ctl_elem_type_t type = snd_ctl_elem_info_get_type(info);
	snd_ctl_elem_value_alloca(&v);
	snd_ctl_elem_value_set_id(v, id);
	if (type == SND_CTL_ELEM_TYPE_BOOLEAN) {
		int on = !strcmp(val, "on") || !strcmp(val, "1");
		for (unsigned i = 0; i < count; i++) snd_ctl_elem_value_set_boolean(v, i, on);
	} else if (type == SND_CTL_ELEM_TYPE_INTEGER) {
		char *end = NULL;
		long x = strtol(val, &end, 10);
		if (end == val || *end) goto out;
		for (unsigned i = 0; i < count; i++) snd_ctl_elem_value_set_integer(v, i, x);
	} else if (type == SND_CTL_ELEM_TYPE_INTEGER64) {
		char *end = NULL;
		long long x = strtoll(val, &end, 10);
		if (end == val || *end) goto out;
		for (unsigned i = 0; i < count; i++) snd_ctl_elem_value_set_integer64(v, i, x);
	} else if (type == SND_CTL_ELEM_TYPE_ENUMERATED) {
		unsigned item = 0, items = snd_ctl_elem_info_get_items(info);
		int found = 0;
		for (; item < items; item++) {
			snd_ctl_elem_info_set_item(info, item);
			const char *nm = snd_ctl_elem_info_get_item_name(info);
			if (nm && !strcmp(nm, val)) { found = 1; break; }
		}
		if (!found) goto out;
		for (unsigned i = 0; i < count; i++) snd_ctl_elem_value_set_enumerated(v, i, item);
	} else goto out;
	if (snd_ctl_elem_write(ctl, v) < 0) goto out;
	r = 0;
out:
	snd_ctl_close(ctl);
	return r;
}

static void cset(int card, const char *name, const char *val)
{
	if (ctl_set(card, name, val)) say("sound: mixer set %s %s -> failed", name, val);
}

/* Is a control registered yet? The codec's controls appear with the SoundWire codec a moment after the card itself, so
 * a set before that fails with "no such control" (the boot that set them without waiting logged all 14 as failed and
 * left the headphone path unset). audio_setup waits on one of them, bounded, before the block that sets them. */
static int ctl_exists(int card, const char *name)
{
	snd_ctl_t *ctl = NULL;
	snd_ctl_elem_id_t *id = NULL;
	snd_ctl_elem_info_t *info = NULL;
	if (ctl_open(card, &ctl) < 0) return 0;
	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, name);
	snd_ctl_elem_id_set_index(id, 0);
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_info_set_id(info, id);
	int r = snd_ctl_elem_info(ctl, info) == 0;
	snd_ctl_close(ctl);
	return r;
}

/* the switch controls (HPHL, HPHL_RDAC, ...) are simple-mixer elements: the same CTL API reaches them */

static int mixer_get(int card, const char *name, char *out, size_t n)
{
	out[0] = 0;
	snd_ctl_t *ctl = NULL;
	snd_ctl_elem_id_t *id = NULL;
	snd_ctl_elem_info_t *info = NULL;
	snd_ctl_elem_value_t *v = NULL;
	if (ctl_open(card, &ctl) < 0) return 0;
	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, name);
	snd_ctl_elem_id_set_index(id, 0);
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_info_set_id(info, id);
	if (snd_ctl_elem_info(ctl, info) == 0) {
		snd_ctl_elem_value_alloca(&v);
		snd_ctl_elem_value_set_id(v, id);
		if (snd_ctl_elem_read(ctl, v) == 0) {
			snd_ctl_elem_type_t type = snd_ctl_elem_info_get_type(info);
			if (type == SND_CTL_ELEM_TYPE_BOOLEAN)
				snprintf(out, n, "%s", snd_ctl_elem_value_get_boolean(v, 0) ? "on" : "off");
			else if (type == SND_CTL_ELEM_TYPE_INTEGER)
				snprintf(out, n, "%ld", snd_ctl_elem_value_get_integer(v, 0));
			else if (type == SND_CTL_ELEM_TYPE_INTEGER64)
				snprintf(out, n, "%lld", snd_ctl_elem_value_get_integer64(v, 0));
			else if (type == SND_CTL_ELEM_TYPE_ENUMERATED) {
				unsigned item = snd_ctl_elem_value_get_enumerated(v, 0);
				if (item < snd_ctl_elem_info_get_items(info)) {
					snd_ctl_elem_info_set_item(info, item);
					snprintf(out, n, "%s", snd_ctl_elem_info_get_item_name(info));
				}
			}
		}
	}
	snd_ctl_close(ctl);
	return (int)strlen(out);
}

/* A sleep: the panel dark and the codec's playback volumes muted before the ADSP's clock stops, so the AMOLED does not
 * flash and the amplifiers do not pop; both put back on the way out. The brightness levels are held here and not saved
 * to the card: the user's own level is untouched. */
static char sleep_bl_name[4][128];
static int sleep_bl_val[4], sleep_bl_n;
static char sleep_rx0[32], sleep_rx1[32];
static void sleep_quiet_begin(void)
{
	char bl[MAX_BL][64];
	int n = backlights(bl, 4);	/* sleep_bl_name[] holds four */
	sleep_bl_n = 0;
	for (int i = 0; i < n; i++) {
		char p[600], mx[600];
		bl_file(p, sizeof p, bl[i], "brightness");
		bl_file(mx, sizeof mx, bl[i], "max_brightness");
		int cur = read_int(p, -1);
		if (cur < 0) continue;
		/* A panel that is already dark (the menu's burn-in guard blanked it, or an earlier sleep) has no level of
		 * its own to come back to, and saving the 0 would wake the handheld to a black screen. Come back to the
		 * user's brightness instead; with nothing written down, leave the panel to the menu's guard. */
		if (cur == 0) {
			int pm = read_int("/data/banana/backlight", -1), max = read_int(mx, 0);
			cur = (pm > 0 && max > 0) ? max * pm / 1000 : -1;
			if (cur == 0) cur = 1;
		}
		if (cur <= 0) continue;
		snprintf(sleep_bl_name[sleep_bl_n], sizeof sleep_bl_name[0], "%.127s", bl[i]);
		sleep_bl_val[sleep_bl_n++] = cur;
		write_file(p, "0");
	}
	if (card_ar >= 0) {
		mixer_get(card_ar, "RX_RX0 Digital Volume", sleep_rx0, sizeof sleep_rx0);
		mixer_get(card_ar, "RX_RX1 Digital Volume", sleep_rx1, sizeof sleep_rx1);
		cset(card_ar, "RX_RX0 Digital Volume", "0");
		cset(card_ar, "RX_RX1 Digital Volume", "0");
	}
}
static void sleep_quiet_end(void)
{
	for (int i = 0; i < sleep_bl_n; i++) {
		char p[600], v[32];
		bl_file(p, sizeof p, sleep_bl_name[i], "brightness");
		snprintf(v, sizeof v, "%d", sleep_bl_val[i]);
		write_file(p, v);
	}
	sleep_bl_n = 0;
	if (card_ar >= 0) {
		if (sleep_rx0[0]) cset(card_ar, "RX_RX0 Digital Volume", sleep_rx0);
		if (sleep_rx1[0]) cset(card_ar, "RX_RX1 Digital Volume", sleep_rx1);
	}
	sleep_rx0[0] = sleep_rx1[0] = 0;
}

/* The jack's answer, confirmed without blocking the loop. The WCD937x's MBHC answers before it has measured the socket:
 * on the Retroid Pocket Classic the first read of a boot (and, worse, a first read after a wake) can say "headphones in"
 * with nothing plugged in, and acting on that routes the sound to a codec feeding an empty jack -- a silent handheld,
 * until something else writes the speaker route back. So the first reading is taken now and the main loop re-reads a
 * couple of hundred ms later; two agreeing readings are believed. A real plug-in still takes effect within a few hundred
 * ms, and its own switch event drives the same variable. (This used to sleep here, holding the loop for up to 1.2 s.) */
static double jack_settle_at;		/* when the loop takes the next reading (mono seconds; 0: none pending) */
static int jack_settle_val, jack_settle_tries;

static void jack_settle_begin(void)
{
	jack_settle_val = headphones_in();
	jack_settle_tries = 0;
	jack_settle_at = mono() + 0.2;
}

static void jack_settle_poll(void)
{
	jack_settle_at = 0;
	int v = headphones_in();
	if (v != jack_settle_val && ++jack_settle_tries < 6) {
		jack_settle_val = v;			/* not settled: look again in a moment */
		jack_settle_at = mono() + 0.2;
		return;
	}
	if (v != headphones) { headphones = v; audio_route(); }
}

static void audio_route(void)
{
	static int last = -1;
	// say it when the answer moves: on the RPC this is the switch between the speaker I2S and the codec, and a
	// jack that changes its mind with nothing plugged in is exactly what silences the handheld
	if (card_ar >= 0 && last != headphones)
		say("sound: routing to the %s (jack read %s)", headphones ? "headphones" : "speakers",
		    headphones_in() ? "in" : "out");
	last = headphones;
	/* The audioreach card plays at 48 kHz (the rate the DSP graph and the amps expect), so "default"'s playback and
	 * capture are pinned to S16 stereo 48 kHz: every open gets the same stream instead of renegotiating it, and
	 * ALSA's plug resamples a core that asks for another rate. */
	/* Everything that opens ALSA's "default" -- the menu, the launchers, the games -- goes to the sound server
	 * (PipeWire), which owns the card through ACP and the UCM2 profile in /usr/share/alsa/ucm2. The routing and
	 * the levels are ACP's then, so the jack switch is a device choice there, not an "hw:N" here. bananad still
	 * writes this file because /etc/asound.conf points at it and every app opens it afresh. */
	char conf[256];
	int n;
	n = snprintf(conf, sizeof conf,
			"# bananad: sound plays through the server (PipeWire)\n"
			"pcm.!default { type pipewire }\n"
			"ctl.!default { type pipewire }\n");
	/* snprintf returns what it would have written, not what it did: clamp before the buffer goes to fwrite, or a
	 * truncated (or negative) n would read past conf */
	if (n < 0) n = 0;
	if ((size_t)n >= sizeof conf) n = (int)sizeof conf - 1;
	FILE *f = fopen("/run/banana/asound.conf.new", "w");
	if (f) {
		fwrite(conf, 1, (size_t)n, f);
		fclose(f);
		rename("/run/banana/asound.conf.new", "/run/banana/asound.conf");
	}
	/* the control the volume keys change, for the settings screen's Volume (the volume script's sset) */
	f = fopen("/run/banana/mixer.new", "w");
	if (f) {
		const char *mix = cfg.mixer;
		if (!strcmp(mix, "auto"))
			/* On the RPC the volume that is *heard* is the two AW88166 amplifiers' own gain -- the kernel
			 * exposes them as "Left PCM"/"Right PCM", where 0 is mute and 511 is 0 dB, the loudest -- not the
			 * DSP's per-stream volume, which the graph latches when it is instantiated, so changing it
			 * mid-stream does nothing. "PCM" is the name the volume script expands to those two controls; it
			 * drives both. */
			mix = card_ar >= 0 ? "PCM" : "Master";
		fprintf(f, "%d %s\n", card_play, mix);
		fclose(f);
		rename("/run/banana/mixer.new", "/run/banana/mixer");
	}
	/* a running RetroArch game opens its sound again where it plays now: alsa-lib reads its configuration once per
	 * process (Banana's RetroArch patch forgets it on AUDIO_REINIT). The menu's own sound watches the file itself. */
	ra_cmd("AUDIO_REINIT");
	say("sound: playback on card %d (%s), 48 kHz", card_play, headphones ? "headphones" : "speakers");
}

/* Is this the Retroid Pocket Classic? Its AudioReach card is the only one whose mixer controls (the frontends'
 * "SENARY_..." switches) appear a moment after the card binds; on any other machine there is nothing to wait for and
 * waiting would only stall the start. Read once. */
static int on_rpc(void)
{
	static int r = -1;
	if (r < 0) {
		r = 0;
		FILE *f = fopen("/proc/device-tree/compatible", "rb");
		if (f) {
			char b[256];
			size_t n = fread(b, 1, sizeof b - 1, f);
			fclose(f);
			b[n] = 0;
			for (char *p = b; p < b + n; p++) if (!*p) *p = ' ';
			if (strstr(b, "sm4450") || strstr(b, "sm8450")) r = 1;
		}
	}
	return r;
}

static void audio_setup(void)
{
	audio_find_cards();
	/* rcS starts us right after the card appears and the DSP makes its audioreach controls a moment later, so on the
	 * RPC look again for a few seconds rather than give the route and the "PCM" level up on the first look. On any
	 * other machine there is no such control to wait for, and waiting would only stall the start. */
	for (int i = 0; on_rpc() && card_ar < 0 && i < 40; i++) { sleep_us(100000); audio_find_cards(); }
	if (card_play < 0) { audio_route(); return; }	/* no sound card at all (QEMU, a host with none): nothing to set */
	if (card_ar < 0) say("sound: no audioreach card (the volume keys have nothing to drive)");
	else say("sound: the audioreach card is %d", card_ar);
	audio_route();
	/* The RPC's DSP latches its routes when a graph is instantiated, and the sound server (ACP + the UCM2 profile)
	 * owns them now. The codec's own DAPM switches below are still re-asserted here, belt-and-braces: bananad runs
	 * after rcS and after restore_settings() replays asound.state, and a card that enumerated late would otherwise
	 * run at the reset levels -- silent or very quiet -- all session. Then the amps' level -- not the frontend's
	 * `stream0.vol_ctrl0 ...`, which is the user's volume (the volume keys and the saved setting own it). */
	if (card_ar >= 0) {
		/* One line every boot, because this codec's jack is what decides where sound goes and it is known to answer
		 * before it has measured anything: "headphones in" here with nothing plugged in is the whole story of a
		 * silent menu -- the sound server's device choice follows the same stale answer. */
		say("sound: the jack says %s at start", headphones ? "headphones in" : "nothing in");
		jack_log();	/* the per-device switch, next to the answer bananad acts on */
		/* The codec's DAPM controls (the switches and muxes below) can be registered a moment after the card's
		 * simple-mixer controls, so a set before that fails ("no such control") and the headphone path is left
		 * unset. Wait for one of them, bounded, and say so if it never appears. */
		int have = 0;
		for (int i = 0; i < 25 && !(have = ctl_exists(card_ar, "RX_MACRO RX0 MUX")); i++) sleep_us(200000);
		if (!have) say("sound: the codec's mux controls are not up: the headphone path may be unset");
		/* The codec's and the RX macro's output volumes and the mic's: the UCM2 profile's own BootSequence levels,
		 * re-asserted here in case the card enumerated after rcS ran (a late card would otherwise sit at the reset
		 * levels -- silent or very quiet -- all session). */
		cset(card_ar, "RX_RX0 Digital Volume", "84");
		cset(card_ar, "RX_RX1 Digital Volume", "84");
		cset(card_ar, "HPHL Volume", "20");
		cset(card_ar, "HPHR Volume", "20");
		cset(card_ar, "ADC2 Volume", "10");
		/* The amplifiers' level is the user's volume (the volume keys and Settings > Volume own it, and
		 * bananad's own restore_settings() has just put the card's saved state back). Only a card that has no
		 * saved level -- a fresh one -- gets Banana's starting point; setting it here every boot was what made
		 * the volume jump back to it. */
		if (access("/data/banana/asound.state", R_OK) != 0) {
			cset(card_ar, "Left PCM Playback Volume", "460");
			cset(card_ar, "Right PCM Playback Volume", "460");
		}
		/* The headphone path, set here rather than only in rcS: bananad's restore_settings() replays the card's
		 * asound.state (saved by alsactl store when the volume changed) after rcS has run, which puts these back to
		 * their reset state, so rcS's own copy is not enough. The speaker path (SENARY MI2S -> AW88166) is separate
		 * and does not need them. */
		cset(card_ar, "HPHL", "on");
		cset(card_ar, "HPHR", "on");
		cset(card_ar, "HPHL_RDAC", "on");
		cset(card_ar, "HPHR_RDAC", "on");
		cset(card_ar, "HPHL_COMP", "on");
		cset(card_ar, "HPHR_COMP", "on");
		cset(card_ar, "RX HPH Mode", "CLS_H_LOHIFI");
		cset(card_ar, "RX_HPH PWR Mode", "LOHIFI");
		cset(card_ar, "RX_MACRO RX0 MUX", "AIF1_PB");
		cset(card_ar, "RX_MACRO RX1 MUX", "AIF1_PB");
		cset(card_ar, "RX INT0_1 MIX1 INP0", "RX0");
		cset(card_ar, "RX INT1_1 MIX1 INP0", "RX1");
		cset(card_ar, "RX INT0 DEM MUX", "CLSH_DSM_OUT");
		cset(card_ar, "RX INT1 DEM MUX", "CLSH_DSM_OUT");
	}
}

static void volume(int dir)
{
	/* One volume, and it is the amplifiers' -- the system's, the way ROCKNIX's keys drive the default sink. The
	 * keys change the same control whether the menu or a game is on top, and a game plays through it, so there is
	 * nothing for "in a game" to mean differently. The DSP's per-stream gain cannot be this (a frontend's graph
	 * latches it at the open), and RetroArch's own volume is not the system's either: a game left at 150% would
	 * come back there however quiet the menu had been. */
	char step[16];
	snprintf(step, sizeof step, "%d", cfg.volume_step);
	char cmd[96];
	snprintf(cmd, sizeof cmd, "/usr/libexec/banana/volume %s %s 2>/dev/null", dir > 0 ? "up" : "down", step);
	save_volume_at = mono() + 2;
	int pct = -1;
	FILE *p = popen(cmd, "r");	/* (it prints the new level) */
	if (p) {
		if (fscanf(p, "%d", &pct) != 1) pct = -1;
		pclose(p);
	}
	if (pct < 0) return;
	/* The level is shown where the menu can draw it, and nowhere else: banana-ui's own OSD -- the pill at the top of
	 * the panel -- in the menu, and nothing over a game. banana-ui is not drawing while cage and RetroArch own the
	 * panel, and the only thing that could draw over a game is RetroArch's own message box ("Volume: N%"), a
	 * different look entirely, so a game gets no indicator rather than a jarring one. (Writing /run/banana/osd during
	 * a game would only surface, stale, when the menu came back.) The volume itself still works in a game: this is
	 * the same amplifier level the menu's pill shows. */
	if (pid_of("/run/banana/game.pid") <= 0)
		osd("volume", pct);
}

static void brightness(int dir, int step_pct)
{
	char bl[MAX_BL][64];
	int n = backlights(bl, MAX_BL), pct = -1;
	for (int i = 0; i < n; i++) {
		char p[320], v[32];
		bl_file(p, sizeof p, bl[i], "max_brightness");
		int max = read_int(p, 0);
		bl_file(p, sizeof p, bl[i], "brightness");
		int cur = read_int(p, 0);
		int step = max * step_pct / 100;
		if (step < 1) step = 1;
		int floor = max * 2 / 100;	/* a night-mode floor: 2%, dim but never a dead-looking panel */
		if (floor < 1) floor = 1;
		int nv = cur + dir * step;
		if (nv < floor) nv = floor;
		if (nv > max) nv = max;
		snprintf(v, sizeof v, "%d", nv);
		write_file(p, v);
		if (max > 0) pct = nv * 100 / max;
	}
	save_backlight_at = mono() + 2;
	if (pct >= 0) osd("brightness", pct);
}

/* both backlights at a fraction of their maximum */
static void backlight_set_permille(int pm)
{
	char bl[MAX_BL][64];
	int n = backlights(bl, MAX_BL);
	for (int i = 0; i < n; i++) {
		char p[320], v[32];
		bl_file(p, sizeof p, bl[i], "max_brightness");
		int max = read_int(p, 0);
		if (max <= 0) continue;
		int nv = max * pm / 1000;
		if (nv < 1) nv = 1;
		bl_file(p, sizeof p, bl[i], "brightness");
		snprintf(v, sizeof v, "%d", nv);
		write_file(p, v);
	}
}

/* the first backlight's level, in thousandths of its maximum */
static int backlight_permille(void)
{
	char bl[MAX_BL][64];
	int n = backlights(bl, MAX_BL), r = -1;
	for (int i = 0; i < n && r < 0; i++) {
		char p[320];
		bl_file(p, sizeof p, bl[i], "max_brightness");
		int max = read_int(p, 0);
		bl_file(p, sizeof p, bl[i], "brightness");
		if (max > 0) r = read_int(p, 0) * 1000 / max;
	}
	return r;
}

/* the panel's backlight, from the backlight-path script. The RPC has one panel and one backlight, and the script
 * returns it whatever name it is asked for ("panel" here). While banana-ui is managing its own idle dim (it writes
 * /run/banana/panel-ui, see ui/src/main.cpp), the menu's own dim leaves that backlight to it: it would otherwise
 * dim twice and undo banana-ui's restore. Only the name is kept: the empty string means nothing to single out. */
static void find_panel_bl(void)
{
	FILE *p = popen("/usr/libexec/banana/backlight-path panel 2>/dev/null", "r");
	if (!p) return;
	char path[256] = "";
	if (fgets(path, sizeof path, p)) path[strcspn(path, "\n")] = 0;
	pclose(p);
	if (!path[0] || strncmp(path, "/sys/class/backlight/", 21)) return;
	const char *s = strrchr(path, '/');
	if (!s || !s[1]) return;
	snprintf(panel_bl, sizeof panel_bl, "%s", s + 1);
	/* published for banana-ui, whose idle timer dims the panel itself (ui/src/main.cpp) */
	FILE *f = fopen("/run/banana/backlight-panel.new", "w");
	if (f) {
		fprintf(f, "%s\n", path);
		fclose(f);
		rename("/run/banana/backlight-panel.new", "/run/banana/backlight-panel");
	}
	say("panel backlight: %s", panel_bl);
}

/* idle in the menu: the panel down to dim_percent of what it was, back at the first press (the level is
 * remembered here, not saved: the user's brightness is untouched) */
static int dimmed, dim_count;
static char dim_name[4][256];
static int dim_level[4];
static void dim(void)
{
	char bl[MAX_BL][64];
	int n = backlights(bl, 4);	/* dim_name[] holds four */
	dim_count = 0;
	for (int i = 0; i < n; i++) {
		if (panel_bl[0] && !strcmp(bl[i], panel_bl) && !access("/run/banana/panel-ui", F_OK)) continue;
		char p[600], v[32];
		snprintf(dim_name[dim_count], sizeof dim_name[0], "%.255s", bl[i]);
		bl_file(p, sizeof p, dim_name[dim_count], "brightness");
		int cur = read_int(p, -1);
		if (cur < 0) continue;
		dim_level[dim_count++] = cur;
		int nv = cur * cfg.dim_pct / 100;
		if (nv < 1) nv = 1;
		snprintf(v, sizeof v, "%d", nv);
		write_file(p, v);
	}
	dimmed = dim_count > 0;
	/* the level before dimming, for the settings screen (opened by the press that undims) */
	if (dimmed) {
		char p[600];
		bl_file(p, sizeof p, dim_name[0], "max_brightness");
		int max = read_int(p, 0);
		if (max > 0) { char v[16]; snprintf(v, sizeof v, "%d\n", dim_level[0] * 1000 / max); write_file("/run/banana/dim", v); }
	}
}
static void undim(void)
{
	if (!dimmed) return;
	for (int i = 0; i < dim_count; i++) {
		char p[600], v[32];
		bl_file(p, sizeof p, dim_name[i], "brightness");
		snprintf(v, sizeof v, "%d", dim_level[i]);
		write_file(p, v);
	}
	dimmed = 0;
	unlink("/run/banana/dim");
}

static void save_settings(double now)
{
	if (save_backlight_at > 0 && now >= save_backlight_at) {
		save_backlight_at = 0;
		int pm = backlight_permille();
		if (pm >= 0) {	/* no backlight (or no readable max): keep the last good level */
			char v[16];
			snprintf(v, sizeof v, "%d\n", pm);
			FILE *f = fopen("/data/banana/backlight", "w");
			if (f) { fputs(v, f); fclose(f); }
		}
	}
	if (save_volume_at > 0 && now >= save_volume_at) {
		save_volume_at = 0;
		const char *path = "/data/banana/asound.state", *tmp = "/data/banana/asound.state.new";
		char *argv[] = { "alsactl", "-f", (char *)tmp, "store", NULL };
		if (run(argv) || rename(tmp, path)) unlink(tmp);	/* a power cut leaves the old state, not a short one */
	}
}

static void restore_settings(void)
{
	int pm = read_int("/data/banana/backlight", -1);
	if (pm > 0 && pm <= 1000) backlight_set_permille(pm);
	/* started again while the panel was dimmed (/run/banana/dim: the level before): undimmed, as the last one would */
	int dimmed_from = read_int("/run/banana/dim", -1);
	if (dimmed_from > 0 && dimmed_from <= 1000) { backlight_set_permille(dimmed_from); unlink("/run/banana/dim"); }
	if (access("/data/banana/asound.state", R_OK) == 0) {
		char *argv[] = { "alsactl", "-f", "/data/banana/asound.state", "restore", NULL };
		run(argv);
	}
}

/* the battery: its directory under /sys/class/power_supply, capacity and whether it charges */
static int battery_state(char *name, size_t n, int *pct, int *charging)
{
	DIR *d = opendir("/sys/class/power_supply");
	if (!d) return 0;
	struct dirent *e;
	int found = 0;
	while (!found && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[320], s[32];
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
		read_str(p, s, sizeof s);
		if (strcmp(s, "Battery")) continue;
		snprintf(name, n, "%s", e->d_name);
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
		*pct = read_int(p, -1);
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
		read_str(p, s, sizeof s);
		*charging = !strcmp(s, "Charging");
		found = 1;
	}
	closedir(d);
	return found;
}

/* a charger plugged in: a power supply that isn't the battery (USB, Mains) and is online. The battery's own status is
 * no use (the amber charger light on ROCKNIX's handhelds lit and went out again): the gauge says "Charging" only for a
 * moment after the cable goes in, then "Discharging" or "Not charging" while it still charges. */
static int charger_online(void)
{
	DIR *d = opendir("/sys/class/power_supply");
	if (!d) return 0;
	struct dirent *e;
	int on = 0;
	while (!on && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[320], s[32];
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
		read_str(p, s, sizeof s);
		if (!strcmp(s, "Battery")) continue;
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/online", e->d_name);
		on = read_int(p, 0) > 0;
	}
	closedir(d);
	return on;
}

/* ---- the vibrator ----------------------------------------------------------------------------------------------- */
static int vib_fd = -1;		/* the FF input, held open: an effect belongs to the fd that uploaded it */
static int vib_id = -1;		/* the uploaded FF_RUMBLE effect, reused between pulses (no per-pulse re-upload) */
static int vib_warned;		/* "no FF input" said once, not on every cable */
static double vib_off_at;	/* when the running pulse stops (mono seconds; 0: none) */
static void vib_open(void);
static void vib_stop(void);

/* The vibrator is the PMIC's PM7250B motor, a module in the image (CONFIG_INPUT_PM8XXX_VIBRATOR=m) that nothing loads
 * at boot. modprobe it once so the FF input (pm8xxx_vib_ffmemless) exists; already loaded is a no-op. */
static void vibrator_load(void)
{
	char *argv[] = { "modprobe", "pm8xxx_vibrator", NULL };
	run(argv);
	vib_open();
}

/* The FF input, held open: an effect belongs to the fd that uploaded it, so opening and closing per pulse (or letting
 * another holder take the device) is how a pulse silently does nothing. The device's name carries "vib"
 * (pm8xxx_vib_ffmemless) and its event number depends on what else probed that boot, so the number does not matter. */
static void vib_open(void)
{
	if (vib_fd >= 0) return;
	DIR *d = opendir("/dev/input");
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "event", 5)) continue;
		char p[300], name[64] = "";
		snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
		int f = open(p, O_RDWR | O_CLOEXEC);
		if (f < 0) continue;
		ioctl(f, EVIOCGNAME(sizeof name), name);
		if (strstr(name, "vib")) { vib_fd = f; vib_id = -1; say("vibrator: %s", e->d_name); break; }
		close(f);
	}
	closedir(d);
	if (vib_fd < 0 && !vib_warned) { vib_warned = 1; say("vibrator: no FF input found"); }
}

/* a short pulse on the vibrator: the FF_RUMBLE effect is uploaded once and reused (id >= 0 updates it in place), played,
 * and stopped on the main loop's own clock (vib_off_at) -- never with a sleep, which would stall input and the
 * charger edge for the pulse's length */
static void vibrate(int ms)
{
	vib_open();
	if (vib_fd < 0) return;
	if (skip("vib")) { say("vibrator: pulse suppressed (sleep_skip)"); return; }	/* debug: prove the edge without buzzing */
	struct ff_effect fx;
	memset(&fx, 0, sizeof fx);
	fx.type = FF_RUMBLE;
	fx.id = vib_id;			/* -1 the first time (allocate); its own id after (update) */
	fx.replay.length = (unsigned short)ms;
	fx.u.rumble.strong_magnitude = 0xffff;
	fx.u.rumble.weak_magnitude = 0xffff;
	if (ioctl(vib_fd, EVIOCSFF, &fx) < 0) {
		/* the device went away (module reloaded) or another holder took the effect: start the fd over once */
		say("vibrator: EVIOCSFF failed (%s), reopening", strerror(errno));
		close(vib_fd);
		vib_fd = -1;
		vib_id = -1;
		vib_warned = 0;
		vib_open();
		if (vib_fd < 0) return;
		fx.id = -1;
		if (ioctl(vib_fd, EVIOCSFF, &fx) < 0) { say("vibrator: upload failed (%s)", strerror(errno)); return; }
	}
	vib_id = fx.id;
	struct input_event ev;
	memset(&ev, 0, sizeof ev);
	ev.type = EV_FF;
	ev.code = fx.id;
	ev.value = 1;
	if (write(vib_fd, &ev, sizeof ev) != (ssize_t)sizeof ev) { say("vibrator: play failed (%s)", strerror(errno)); return; }
	vib_off_at = mono() + ms / 1000.0;
}

/* stop a running pulse; called from the main loop when vib_off_at comes due */
static void vib_stop(void)
{
	if (vib_fd < 0 || vib_id < 0) return;
	struct input_event ev;
	memset(&ev, 0, sizeof ev);
	ev.type = EV_FF;
	ev.code = vib_id;
	ev.value = 0;
	if (write(vib_fd, &ev, sizeof ev) < 0) {}
}

/* The power state: a battery reading, the charger's edge (the plug pulse) and one log line when either moves. The
 * handheld has no LEDs, so there is nothing to drive; this runs on the power_supply uevents and the battery poll. */
static void power_update(void)
{
	char bat[256] = "";
	int pct = -1, charging = 0;
	if (!battery_state(bat, sizeof bat, &pct, &charging)) pct = 100;
	int plugged = charger_online();
	static int prev_plugged = -1;
	if (prev_plugged >= 0 && prev_plugged != plugged) say("charger %s", plugged ? "plugged in" : "unplugged");
	if (prev_plugged == 0 && plugged) vibrate(cfg.vibrate_ms);	/* the cable went in (not at boot, nor on unplug) */
	prev_plugged = plugged;
	if (plugged) charging = 1;	/* plugged in counts as charging (see charger_online) */
	static int last_pct = -1, last_chg = -1;
	if (pct != last_pct || charging != last_chg) {
		last_pct = pct;
		last_chg = charging;
		say("battery %d%%%s", pct, charging ? ", charging" : "");
	}
}

/* the kernel's uevents (no udev here): the battery and the charger, and drivers for USB devices plugged in */
static int uevent_open(void)
{
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	if (fd < 0) return -1;
	struct sockaddr_nl a = { .nl_family = AF_NETLINK, .nl_groups = 1 };
	if (bind(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
	return fd;
}

/* a program in the background, not waited for (a double fork: no zombie, SIGCHLD stays default) */
static void spawn_bg(char *const argv[])
{
	pid_t p = fork();
	if (p == 0) {
		if (fork() == 0) { execv(argv[0], argv); _exit(127); }
		_exit(0);
	}
	if (p > 0) while (waitpid(p, NULL, 0) < 0 && errno == EINTR) {}
}

/* a USB device's driver, as udev would load it: the module its MODALIAS names (an Xbox pad's xpad, a USB serial
 * adapter's); the handheld's own hardware is built into the kernel */
static void load_driver(const char *alias)
{
	if (strncmp(alias, "usb:", 4)) return;
	char a[256];
	snprintf(a, sizeof a, "%s", alias);
	char *argv[] = { "/sbin/modprobe", "-q", a, NULL };
	spawn_bg(argv);
}

/* the USB devices there before bananad started (a pad plugged in at boot) */
static void load_drivers_present(void)
{
	DIR *d = opendir("/sys/bus/usb/devices");
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[320], alias[256];
		snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/modalias", e->d_name);
		FILE *f = fopen(p, "r");
		if (!f) continue;
		if (fgets(alias, sizeof alias, f)) { alias[strcspn(alias, "\n")] = 0; load_driver(alias); }
		fclose(f);
	}
	closedir(d);
}

/* the second SD card: mounted, and its games offered to the library, to match the slot (/usr/libexec/banana/sd2).
 * In the background (a slow card's mount mustn't hold up the power button or sleep), one scan at a time: a card
 * going in sends several uevents */
static void sd2_scan(void)
{
	char *argv[] = { "/bin/sh", "-c", "flock /run/banana/sd2.lock /usr/libexec/banana/sd2 scan </dev/null >/dev/null 2>&1 &", NULL };
	run(argv);
}

/* the kernel's uevents: true when one was about the battery or the charger; a device added gets its driver */
static int uevent_read(int fd)
{
	char buf[8192];
	ssize_t n;
	int hit = 0;
	while ((n = recv(fd, buf, sizeof buf - 1, 0)) > 0) {
		buf[n] = 0;
		const char *alias = NULL;
		int add = 0, block = 0, mmc = 0, udc = 0;
		for (char *p = buf; p < buf + n; p += strlen(p) + 1) {
			if (!strcmp(p, "SUBSYSTEM=power_supply")) hit = 1;
			else if (!strcmp(p, "SUBSYSTEM=block")) block = 1;
			else if (!strcmp(p, "SUBSYSTEM=udc")) udc = 1;
			else if (!strncmp(p, "DEVNAME=mmcblk", 14)) mmc = 1;
			else if (!strcmp(p, "ACTION=add")) add = 1;
			else if (!strncmp(p, "MODALIAS=", 9)) alias = p + 9;
		}
		if (add && alias) load_driver(alias);
		if (block && mmc) sd2_scan();	/* a card in or out of the second slot (the script looks at what's there) */
		if (add && udc && !msc_rw()) {	/* an OTG role switch back to peripheral: rcS's one-shot run cannot see this */
			/* one role switch can send several add events, and two usb-gadget runs at once tear the gadget
			 * down; one run every couple of seconds is plenty */
			static double last_udc;
			double u = mono();
			if (u - last_udc >= 2.0) {
				last_udc = u;
				char *argv[] = { "/usr/libexec/banana/usb-gadget", NULL };
				spawn_bg(argv);
			}
		}
	}
	return hit;
}

static void open_inputs(void)
{
	for (int i = 0; i < ndev; i++) close(devfd[i]);
	ndev = 0;
	DIR *d = opendir("/dev/input");
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) && ndev < MAXDEV) {
		if (strncmp(e->d_name, "event", 5)) continue;
		char path[300];
		snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) continue;
		/* only keys and switches reach us (handle() reads nothing else): the touchscreens' and the sticks' motion during
		 * a game woke bananad 60-200 times a second for nothing. The kernel drops a report left empty, so no wake;
		 * EV_SYN stays, its report is what wakes us for the rest (no EVIOCSMASK, an old kernel: everything, as before) */
		unsigned long types = (1ul << EV_SYN) | (1ul << EV_KEY) | (1ul << EV_SW);
		struct input_mask m = { .type = 0, .codes_size = sizeof types, .codes_ptr = (uint64_t)(uintptr_t)&types };
		ioctl(fd, EVIOCSMASK, &m);
		devfd[ndev++] = fd;
	}
	closedir(d);
}

/* A command to a running RetroArch game: its stdin command interface, through the FIFO run-retroarch holds open
 * (bananad's volume keys already write to it). Nothing happens when no game runs -- the FIFO is not there. */
static void ra_cmd(const char *cmd)
{
	int fd = open("/run/banana/retroarch.cmd", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return;
	if (write(fd, cmd, strlen(cmd)) < 0) {}
	if (write(fd, "\n", 1) < 0) {}
	close(fd);
}

static void handle(const struct input_event *e)
{
	double now = mono();
	if (e->type == EV_KEY) { last_input = now; undim(); if (panel_blanked) { panel_blanked = 0; backlights_power(1); } }
	if (e->type == EV_SW && e->code == SW_HEADPHONE_INSERT) {
		if (headphones != e->value) { headphones = e->value; audio_route(); }
		return;
	}
	if (e->type != EV_KEY) return;
	/* a volume key held down: the kernel's autorepeat (gpio-keys-volume's), a step every 150 ms so the level card keeps
	 * up; every other key's repeats mean nothing here */
	if (e->value == 2) {
		static double last_repeat;
		if ((e->code != KEY_VOLUMEUP && e->code != KEY_VOLUMEDOWN) || now - last_repeat < 0.15) return;
		last_repeat = now;
	}
	switch (e->code) {
	case BTN_MODE: fn_held = e->value; break;
	case BTN_SELECT: select_held = e->value; break;
	/* F (MODE, the button under the screen) + B, in a running game: the d-pad toggles between the d-pad and the
	 * analogue stick (RetroArch's TOGGLE_ANALOG_DPAD, the command banana's RetroArch patch adds). The RPC has no
	 * analogue stick, so this is what lets a game that reads only the stick -- a Dreamcast racer -- be played with
	 * the d-pad, and hands the d-pad back for a game that wants it as a d-pad. */
	case BTN_SOUTH: if (e->value && fn_held) ra_cmd("TOGGLE_ANALOG_DPAD"); break;
	case KEY_POWER:
		if (now < ignore_power_until) break;
		/* Transfer mode (Settings > System > Mass storage, read-write): the menu is stopped and /data is the
		 * computer's. A press brings the card back -- usb-msc off ejects the disk from the host, mounts /data and
		 * restarts the menu -- rather than sleeping. Eject on the computer first if it is safe to do so. */
		if (e->value && msc_rw()) {
			char *argv[] = { "/usr/libexec/banana/usb-msc", "off", NULL };
			spawn_bg(argv);
			break;
		}
		/* The menu's burn-in guard has the screen dimmed or blanked (/run/banana/screen-off): this press is to wake
		 * it, so it must not also start a sleep -- two things at once is what makes waking it feel broken. The next
		 * press, screen on, sleeps it as usual. */
		if (e->value && access("/run/banana/screen-off", F_OK) == 0) break;
		if (e->value) { power_down = 1; power_since = now; }
		else if (power_down) { power_down = 0; suspend_now("power button"); }
		break;
	/* F + volume: the backlight, in games too. Select + volume: the same in a game (in the menu Select is the
	 * search key), in 10% steps down to a 2% night-mode floor. */
	case KEY_VOLUMEUP:
		if (e->value) { int bright = fn_held || (select_held && pid_of("/run/banana/game.pid") > 0); if (bright) brightness(1, select_held && !fn_held ? 10 : cfg.brightness_step); else volume(1); }
		break;
	case KEY_VOLUMEDOWN:
		if (e->value) { int bright = fn_held || (select_held && pid_of("/run/banana/game.pid") > 0); if (bright) brightness(-1, select_held && !fn_held ? 10 : cfg.brightness_step); else volume(-1); }
		break;
	}
}

/* percent, or -1 when there is no battery or it is charging */
/* the gauge's charge counter in uAh (-1 on the charger, like the percentage): the number a sleep is measured by --
 * one capacity step is 1% of 5 Ah, far too coarse for a few hours */
static long battery_charge_uah(void)
{
	if (charger_online()) return -1;
	DIR *d = opendir("/sys/class/power_supply");
	if (!d) return -1;
	struct dirent *e;
	long r = -1;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[320], s[32];
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
		read_str(p, s, sizeof s);
		if (strcmp(s, "Battery")) continue;
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/charge_counter", e->d_name);
		r = (long)read_int(p, -1);
		break;
	}
	closedir(d);
	return r;
}

static int battery_discharging_pct(void)
{
	/* (the gauge says "Discharging" while a charger charges it, see charger_online: plugged in at 2% must not
	 * shut down) */
	if (charger_online()) return -1;
	DIR *d = opendir("/sys/class/power_supply");
	if (!d) return -1;
	struct dirent *e;
	int r = -1;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[320], s[32];
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
		read_str(p, s, sizeof s);
		if (strcmp(s, "Battery")) continue;
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
		read_str(p, s, sizeof s);
		if (!strcmp(s, "Discharging")) {
			snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
			r = read_int(p, -1);
		}
		break;
	}
	closedir(d);
	return r;
}

/* The idle timer of a slate (the Retroid Pocket Classic). There is nothing to close, so an idle suspend has no
 * meaning -- and on this SoC it still costs the picture: the DSI panel and its backlight resume is a gap ROCKNIX has
 * not closed either. So an idle handheld dims and stays awake, which is the battery saving you can see coming. The
 * power button still sleeps it, and writing autosleep_minutes in /data/banana/bananad.conf (Settings > Power) asks
 * for the timer back: the image's own default is not an ask. */
static void slate_sleep_default(void)
{
	if (cfg.autosleep_set || cfg.autosleep_min <= 0)
		return;
	say("sleep: idle only dims (%g min), never suspends; autosleep_minutes asks for the timer", cfg.dim_min);
	cfg.autosleep_min = 0;
}

/* Periodic USB housekeeping: (1) log the Type-C roles when they change, to diagnose a reverse-orientation plug
 * (syslogd -> /run/banana/messages); (2) re-bind the gadget when a port role flip cleared its UDC. A reverse
 * hotplug drops the gadget -- configfs UDC goes empty and /sys/class/udc reads "not attached" -- and nothing
 * re-binds it (there is no UDC "add" event), so the host sees nothing even though the port, PHY and mux are fine.
 * Writing the UDC back recovers it (verified on the unit); it is harmless at boot and while the cable is out. */
static void usb_housekeeping(double now)
{
	static double next;
	static char said[48];
	if (now < next) return;
	next = now + 3;
	DIR *d = opendir("/sys/class/udc");
	if (!d) return;
	int configured = 0;
	char udcname[64] = "";
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		if (!udcname[0]) snprintf(udcname, sizeof udcname, "%.63s", e->d_name);
		char p[300], st[32];
		snprintf(p, sizeof p, "/sys/class/udc/%s/state", e->d_name);
		read_str(p, st, sizeof st);
		if (!strcmp(st, "configured")) configured = 1;
	}
	closedir(d);
	char dr[32] = "", pr[32] = "", cur[16] = "", pcur[16] = "";
	read_str("/sys/class/typec/port0/data_role", dr, sizeof dr);
	read_str("/sys/class/typec/port0/power_role", pr, sizeof pr);
	sscanf(dr, "%15s", cur);
	sscanf(pr, "%15s", pcur);
	char state[64];
	snprintf(state, sizeof state, "%s/%s/%s", cur[0] ? cur : "?", pcur[0] ? pcur : "?",
	    configured ? "up" : "down");
	if (strcmp(state, said)) {
		snprintf(said, sizeof said, "%.47s", state);
		say("usb: typec data_role=%s power_role=%s controller=%s", cur[0] ? cur : "?",
		    pcur[0] ? pcur : "?", configured ? "configured" : "not configured");
	}
	if (configured || !udcname[0]) return;
	char bound[16] = "";
	read_str("/sys/kernel/config/usb_gadget/banana/UDC", bound, sizeof bound);
	if (bound[0]) return;	/* still bound (unplugged, or a host that never dropped it) */
	if (access("/sys/kernel/config/usb_gadget/banana/functions/ncm.usb0", F_OK) ||
	    access("/sys/kernel/config/usb_gadget/banana/configs/c.1/ncm.usb0", F_OK))
		return;		/* the tree is incomplete: usb-gadget owns the build */
	if (write_file("/sys/kernel/config/usb_gadget/banana/UDC", udcname) == 0)
		say("usb: re-bound the gadget to %s (a port role flip had cleared it)", udcname);
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	openlog("bananad", LOG_PID, LOG_DAEMON);
	load_conf("/etc/banana/bananad.conf", 0);
	load_conf("/data/banana/bananad.conf", 1);
	load_timezone();
	signal(SIGCHLD, SIG_DFL);
	struct sigaction sa = { 0 };
	sa.sa_handler = on_hup;		/* no SA_RESTART: poll() returns, and the loop reloads */
	sigaction(SIGHUP, &sa, NULL);
	mkdir("/run/banana", 0755);
	/* debugfs carries the wakeup_sources table wake_reason() names the waker from (the RPC's pm_wakeup_irq is
	 * absent); mounting it again is a harmless EBUSY if something else already did */
	mkdir("/sys/kernel/debug", 0755);
	if (mount("none", "/sys/kernel/debug", "debugfs", 0, NULL) && errno != EBUSY) {}
	find_panel_bl();	/* which backlight banana-ui's idle dim owns */

	int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	int ino_in = -1, ino_run = -1;
	if (ino >= 0) {
		ino_in = inotify_add_watch(ino, "/dev/input", IN_CREATE | IN_DELETE);
		ino_run = inotify_add_watch(ino, "/run/banana", IN_CREATE | IN_MOVED_TO);	/* the /run/banana/vibrate test trigger */
	}
	open_inputs();
	headphones = headphones_in();	/* the codec's first answer; the loop confirms (and corrects) it (see jack_settle) */
	jack_settle_begin();
	slate_sleep_default();	/* the idle rule: dim but don't suspend */
	/* the saved volumes first, then the route: the restore also brings back the speakers' switch as it was last
	 * saved (off if headphones were in then), and the route sets it from the jack as it is now */
	restore_settings();
	audio_setup();
	int uev = uevent_open();
	sd2_scan();	/* a card already in the second slot at boot */
	load_drivers_present();
	vibrator_load();	/* so the plug-in's pulse has a motor to use (the module is not loaded at boot) */
	power_update();
	last_input = mono();
	double next_battery = 0;
	int low_count = 0;
	say("running: auto-sleep %d min, %s%s", cfg.autosleep_min, cfg.mem_sleep,
	    rtc_wake_possible() ? "" : ", no RTC wake alarm (only power/charger wakes it)");

	for (;;) {
		struct pollfd pf[MAXDEV + 2];
		int n = 0;
		for (int i = 0; i < ndev; i++) pf[n++] = (struct pollfd){ devfd[i], POLLIN, 0 };
		int ino_at = -1, uev_at = -1;
		if (ino >= 0) { ino_at = n; pf[n++] = (struct pollfd){ ino, POLLIN, 0 }; }
		if (uev >= 0) { uev_at = n; pf[n++] = (struct pollfd){ uev, POLLIN, 0 }; }
		/* sleep until something is due: the battery look (a minute), the dim or the auto-sleep deadline, a pending
		 * save, the power button's hold; nothing wakes bananad otherwise */
		double tnow = mono(), wait = next_battery - tnow;
		int game = pid_of("/run/banana/game.pid") != 0;
		/* a job that must not be slept through (banana-scrape's downloads): its pid in /run/banana/awake.pid; looked
		 * at again every 5 s, so the idle sleep comes once it is over */
		int awake = pid_of("/run/banana/awake.pid") != 0;
		if (awake && wait > 5) wait = 5;
		/* (only deadlines still ahead: one already passed was acted on after the last poll, or is held back by what
		 * the action checks (the charger, no backlight) and a uevent or an input event brings it back; counted
		 * here it spun this loop every 20 ms, on the charger for as long as the menu sat idle) */
		if (!game) {
			if (cfg.autosleep_min > 0 && !awake) { double d = cfg.autosleep_min * 60.0 - (tnow - last_input); if (d > 0 && d < wait) wait = d; }
			if (cfg.dim_min > 0 && !dimmed) { double d = cfg.dim_min * 60.0 - (tnow - last_input); if (d > 0 && d < wait) wait = d; }
		}
		if ((save_volume_at > 0 || save_backlight_at > 0) && wait > 0.5) wait = 0.5;
		if (power_down && wait > 0.1) wait = 0.1;
		if (vib_off_at > 0 && vib_off_at - tnow < wait) wait = vib_off_at - tnow;	/* a pulse's stop */
		if (jack_settle_at > 0 && jack_settle_at - tnow < wait) wait = jack_settle_at - tnow;	/* the jack's second look */
		int timeout = wait <= 0 ? 20 : (int)(wait * 1000) + 20;
		poll(pf, n, timeout);
		double now = mono();
		if (vib_off_at > 0 && now >= vib_off_at) { vib_stop(); vib_off_at = 0; }
		if (jack_settle_at > 0 && now >= jack_settle_at) jack_settle_poll();
		if (reload) {
			reload = 0;
			cfg.autosleep_set = 0;
			load_conf("/etc/banana/bananad.conf", 0);
			load_conf("/data/banana/bananad.conf", 1);
			slate_sleep_default();	/* the settings screen may have just written a timer, or taken it away */
			load_timezone();
			save_volume_at = mono() + 2;	/* the settings screen may have set the volume: keep it as the keys' is kept */
			power_update();
			say("settings read again: auto-sleep %d min", cfg.autosleep_min);
		}

		for (int i = 0; i < ndev; i++) {
			if (!(pf[i].revents & POLLIN)) continue;
			struct input_event ev[64];
			ssize_t r;
			while ((r = read(devfd[i], ev, sizeof ev)) > 0)
				for (int k = 0; k < r / (ssize_t)sizeof ev[0]; k++) handle(&ev[k]);
		}
		if (uev_at >= 0 && (pf[uev_at].revents & POLLIN) && uevent_read(uev)) power_update();
		save_settings(now);
		if (ino_at >= 0 && (pf[ino_at].revents & POLLIN)) {
			char buf[4096];
			ssize_t r;
			int inputs_changed = 0, vibrate_req = 0;
			while ((r = read(ino, buf, sizeof buf)) > 0)
				for (char *p = buf; p < buf + r; ) {
					struct inotify_event *e = (struct inotify_event *)p;
					if (e->len && e->wd == ino_run && !strcmp(e->name, "vibrate")) vibrate_req = 1;
					if ((e->mask & (IN_CREATE | IN_DELETE)) && e->wd == ino_in) inputs_changed = 1;
					p += sizeof(struct inotify_event) + e->len;
				}
			if (vibrate_req) { unlink("/run/banana/vibrate"); vibrate(cfg.vibrate_ms); }	/* a test trigger: `touch /run/banana/vibrate` */
			if (inputs_changed) {
				sleep_us(200000);	/* udev/mdev sets the node's permissions */
				open_inputs();
				jack_log();		/* a jack that probed after bananad opened the inputs shows up here, not at start */
				jack_settle_begin();	/* and its state is read again: the boot route may have run before it existed */
			}
		}

		if (power_down && now - power_since >= cfg.power_long_ms / 1000.0) { power_down = 0; shutdown_now("power held"); }

		/* idle in the menu (no game running): the panel dims, then it sleeps */
		game = pid_of("/run/banana/game.pid") != 0;
		if (cfg.dim_min > 0 && !dimmed && !game && now - last_input >= cfg.dim_min * 60.0) dim();
		/* (not while charging: a sleep would put the charging light out; the menu stays dimmed instead) */
		if (cfg.autosleep_min > 0 && !game && now - last_input >= cfg.autosleep_min * 60.0 && !charger_online() &&
		    !pid_of("/run/banana/awake.pid"))
			suspend_now("idle");

		usb_housekeeping(now);	/* log the Type-C roles; re-bind a gadget a role flip cleared */
		if (now >= next_battery) {
			next_battery = now + 60;
			power_update();	/* a capacity change has no uevent on every gauge */
			int pct = battery_discharging_pct();
			low_count = pct >= 0 && pct <= cfg.battery_off_pct ? low_count + 1 : 0;
			if (low_count >= 2) shutdown_now("battery empty");	/* two readings in a row: not a load spike */
		}
	}
}
