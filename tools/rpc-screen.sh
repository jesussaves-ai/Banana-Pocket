#!/bin/sh
# Ask the handheld's menu for the screen it is showing, and fetch it over its USB link.
#
# banana-ui writes the frame as a JPEG when /run/banana/screen.req appears (ui/src/main.cpp, on the inotify watch it
# already keeps on /run/banana for the launch file), so this is a request/response over the card's own files; the
# bytes come back over the USB link the handheld's own gadget provides -- 169.254.170.2, no configuration on this end
# (docs/ARCHITECTURE.md, "USB Device Link", and docs/RPC-TEST.md for the cable). RPC_HOST overrides where to connect
# (e.g. the Wi-Fi address, which differs from machine to machine).
#
#   tools/rpc-screen.sh [out.jpg]      one frame (default /tmp/opencode/screen.jpg)
#   tools/rpc-screen.sh -w             a frame stream, as MJPEG on stdout, for a viewer:
#                                        tools/rpc-screen.sh -w | ffplay -f mjpeg -
#                                        tools/rpc-screen.sh -w | mpv -
#   tools/rpc-screen.sh -w -n 20       the same, but stop after 20 frames (default: until stopped)
#
# The menu has to be showing: with a game running, cage owns the panel and banana-ui has no frame to give.
set -e
H=${RPC_HOST:-root@169.254.170.2}
# Multiplexed: a stream is one process per frame otherwise, and a fresh handshake each time is most of the frame's
# cost (each round trip is a request, a wait and a 147 KB JPEG back).
CTL=${TMPDIR:-/tmp}/opencode
mkdir -p "$CTL" 2>/dev/null || true
SSH="ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=6 -o ControlMaster=auto -o ControlPersist=30 -o ControlPath=$CTL/.rpc-screen-%r@%h:%p"

mode=one
n=0
out=/tmp/opencode/screen.jpg
while [ $# -gt 0 ]; do
	case $1 in
	-w) mode=stream ;;
	-n) shift; n=${1:-0} ;;
	-h|--help) sed -n '2,16p' "$0"; exit 0 ;;
	*) out=$1 ;;
	esac
	shift
done

# Ask for one frame and hand it back on stdout. The whole exchange is one connection: what makes the frame ready is the
# file appearing, and the menu writes it within a frame or two of the request (a JPEG of 1240x1080 takes it ~50 ms).
frame() {
	$SSH "$H" '
		rm -f /run/banana/screen.jpg
		: > /run/banana/screen.req
		i=0
		while [ $i -lt 120 ] && [ ! -s /run/banana/screen.jpg ]; do sleep 0.05; i=$((i+1)); done
		[ -s /run/banana/screen.jpg ] || { echo "rpc-screen: no frame (a game running? no menu?)" >&2; exit 1; }
		exec cat /run/banana/screen.jpg'
}

if [ "$mode" = one ]; then
	d=$(dirname "$out")
	[ "$d" = . ] || mkdir -p "$d"
	frame > "$out"
	echo "$out ($(wc -c < "$out") bytes)"
	exit 0
fi

i=0
while [ "$n" -eq 0 ] || [ "$i" -lt "$n" ]; do
	frame
	i=$((i+1))
done
