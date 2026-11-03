# Testing the sound

Audio on the RPC is a chain of five things that each fail **silently**: the sound server (PipeWire + WirePlumber),
ALSA's `default` (the plugin that hands every client to the server), the *Speaker* sink (ACP's UCM profile), the
AW88166 amplifier's hardware gain (bananad's volume keys), and the AudioReach DSP graph (the frontend the server keeps
running, because this DSP clears its routing when the graph is torn down). A broken link sounds exactly like a broken
speaker, and the only way to tell used to be a reboot and guesswork.

There are two tests, and they check different halves:

| | where | what it proves |
|---|---|---|
| `make test` → `system/tests/sound-config-test.sh` | this computer, in CI | the code and the configuration still obey the sound laws (see below). No hardware needed |
| `make sound-test` → `/usr/libexec/banana/sound-test` | the handheld | the real chain works, now, end to end |

## On the handheld

```
make sound-test                        # from the computer, over ssh (BANANA_HOST, default root@169.254.170.2)
make sound-test ARGS=--listen          # also play the cue three times, to check with your ears
/usr/libexec/banana/sound-test --listen    # or on the handheld itself, with a keyboard/console
```

The script is streamed over ssh (like `tools/rpc-push-update.sh`), so it also tests an image built before the script
was in it. It sets `XDG_RUNTIME_DIR=/run/banana/wayland` itself — without that, ALSA's plugin cannot find the server
and *everything* looks broken.

What it checks, in order, and what a failure means:

1. **the sound server** — `pipewire` and `wireplumber` are running and `/run/banana/wayland/pipewire-0` exists.
   Failure: `/data/banana/logs/sound-server.log`, `/usr/libexec/banana/sound-server status`.
2. **ALSA's default** — `/etc/asound.conf` and `/run/banana/asound.conf` both say `type pipewire`. Failure: clients
   would open the hardware card and fight the server for the frontend.
3. **the sink** — the *Speaker* sink is in PipeWire and is the default. A default of Headphones with nothing in the
   jack is the after-update stall (the Headphones sink is created first; the Speaker never follows): run
   `/usr/libexec/banana/sound-server recover`.
4. **the amplifier** — `amixer -c 0 sget 'Left PCM'` / `'Right PCM'` exist and are not at 0 %. A missing control means
   the AW88166 is not in the UCM profile; 0 % is a warning (it is a saved volume, not a fault).
5. **the DSP graph** — `/proc/asound/card0/pcm0p/sub0/status` is `RUNNING` and owned by pipewire. Anything else means
   the frontend idled out and the DSP dropped its route: silence even though every sink looks fine.
6. **a client** — `aplay -D default` (or mpv's ALSA output where aplay is not in the image) plays the image's own cue,
   and a *stream* shows up at the Speaker sink. This is the same alsa-lib path the menu and the games take.
7. **the menu** — `/run/banana/sound-state` exists, then the sleep handover is exercised for real: `SIGHUP` →
   `closed`, `SIGUSR1` → `open`, each within 2 s. This is the cycle bananad performs before it suspends, and the
   reason 0.5.2 latches a handover: the menu used to reopen the device 10 ms after closing it.

`--listen` plays the cue three times through that same path. With a terminal it asks whether you heard it; streamed,
it just plays. It is the only check that tests the speaker itself.

Exit code is 0 only if every hard check passed; warnings (a 0 % saved volume, a game holding the device) do not fail
it. Each run is appended to `/data/banana/logs/sound-test.log`.

## On the computer (and in CI)

`make test` runs `system/tests/sound-config-test.sh`, which checks the rules that, when broken, cost an update and a
reboot to notice:

- the menu is a PipeWire client: re-reads ALSA's config per open, publishes `/run/banana/sound-state`, latches a
  handover, closes an idle device after 20 s, and never opens a hardware card;
- bananad writes the PipeWire `default` and waits for the handover ack before sleeping;
- no client calls `amixer`/`alsaucm` or touches `hw:0` (outside comments);
- the UCM profile keeps the shared FE↔BE switches in `SectionVerb` — a `SectionDevice` teardown clobbers the active
  route (`AGENTS.md` §5, the DSP law);
- WirePlumber never idle-suspends the node (`session.suspend-timeout-seconds = 0`) and sets the q6apm-safe buffering
  per node under `monitor.alsa.rules` (`monitor.alsa.properties` never reaches ACP's nodes);
- `sound-server` waits for the *Speaker* sink and has a `recover` action; rcS exports the runtime dir the clients use;
- `/etc/asound.conf` is a symlink to `/run/banana/asound.conf` **in the image source**;
- the image would carry the pieces (`pipewire`, `wireplumber`, `alsa-utils`' `aplay`) and the built `banana-ui` links
  `libasound`.

## What the tests have already caught

- The 0.5.2 menu left `/run/banana/sound-state` on `releasing (handover)` for good when the device was *already*
  closed (the loop publishes `closed` only when it has a handle). bananad then waited out its 2 s bound on every
  sleep. Found by stage 7 on the handheld, fixed in `Sound::release()`.
- `/etc/asound.conf` existed only in a hand-modified build tree (`output/.../target`), not in the repo: a clean build
  would have had no default device at all. Found by the config test, now a symlink in `system/rootfs/etc`.

## Limits

The tests cannot hear. `--listen` is the only audibility check, and the headphone jack is only exercised by plugging
headphones in (the test unit has none). A failure in stage 4's *volume* is a saved setting, not a defect.
