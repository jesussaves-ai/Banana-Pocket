#!/usr/bin/env python3
# Draws the theme's system icons at 32x32, in the house style: transparent background, a 1px black outline, flat fills,
# a small grey (or one-hue) ramp and one accent, no anti-aliasing. The RNDS theme reads one PNG per system id from
# ui/assets/rnds/icons (systems.cfg maps a system to its file and accent); this keeps the drawn set reproducible
# instead of hand-edited PNGs.
#
#   tools/gen-icons.py [outdir]      (default: ui/assets/rnds/icons)
#
# Only the icons this script owns are written; the console icons from BenJelter's pack (nds, gba, snes, psx, n64, md,
# arcade) are not touched. A system with no dedicated file falls back to a kind icon in systems.cfg.
import os
import struct
import sys
import zlib

W = H = 32

BG = (0, 0, 0, 0)
OUT = (0, 0, 0, 255)
BLACK = (16, 16, 16, 255)
DARK = (41, 41, 41, 255)
MID = (103, 103, 103, 255)
GREY = (150, 150, 150, 255)
LIGHT = (195, 195, 195, 255)
HI = (225, 225, 225, 255)
WHITE = (255, 255, 255, 255)
BROWN = (176, 112, 58, 255)
BROWND = (122, 74, 36, 255)
BEIGE = (203, 191, 158, 255)
BEIGEL = (233, 223, 196, 255)
BLUE = (61, 111, 184, 255)
RED = (210, 59, 46, 255)
ORANGE = (224, 122, 42, 255)
YELLOW = (224, 192, 32, 255)
GREEN = (61, 174, 82, 255)
TEAL = (46, 125, 120, 255)
CYAN = (68, 212, 202, 255)


class Canvas:
    def __init__(self):
        self.p = [[BG] * W for _ in range(H)]

    def px(self, x, y, c):
        if 0 <= x < W and 0 <= y < H:
            self.p[y][x] = c

    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.px(x, y, c)

    def box(self, x0, y0, x1, y1, fill, line=OUT):
        self.rect(x0, y0, x1, y1, fill)
        for x in range(x0, x1 + 1):
            self.px(x, y0, line)
            self.px(x, y1, line)
        for y in range(y0, y1 + 1):
            self.px(x0, y, line)
            self.px(x1, y, line)

    def disc(self, cx, cy, r, c):
        for y in range(H):
            for x in range(W):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    self.px(x, y, c)

    def ring(self, cx, cy, r, c):
        for y in range(H):
            for x in range(W):
                d = (x - cx) ** 2 + (y - cy) ** 2
                if (r - 1) ** 2 < d <= r * r:
                    self.px(x, y, c)

    def spiral(self, cx, cy, rmax, c, turns=2.4):
        import math
        n = 900
        for i in range(n):
            t = i / n * (turns * 2 * math.pi)
            r = rmax * i / n
            self.px(int(round(cx + r * math.cos(t))), int(round(cy + r * math.sin(t))), c)


# ---- the icons ----------------------------------------------------------------------------------------------------

def gamegear(c):
    # a black wide handheld: d-pad left, a big screen, the two buttons right (the Game Gear's shape)
    c.box(2, 8, 29, 24, BLACK)
    c.rect(3, 9, 28, 23, DARK)
    c.rect(3, 9, 28, 10, MID)                       # a top highlight
    c.box(8, 11, 21, 21, BLACK)                     # screen bezel
    c.rect(9, 12, 20, 20, TEAL)                     # the backlit screen
    c.rect(9, 12, 20, 13, CYAN)
    # the d-pad: a plus
    c.rect(4, 15, 9, 18, OUT)
    c.rect(5, 13, 8, 20, OUT)
    c.rect(5, 16, 8, 17, MID)
    c.rect(6, 14, 7, 19, MID)
    # button 1 and 2, stacked
    c.disc(25, 13, 2, OUT)
    c.disc(25, 13, 1, RED)
    c.disc(25, 18, 2, OUT)
    c.disc(25, 18, 1, BLUE)
    c.px(23, 22, MID)                               # the speaker
    c.px(26, 22, MID)


def dreamcast(c):
    # a light grey console, its top lid carrying the orange swirl
    c.box(2, 13, 29, 25, LIGHT)
    c.rect(3, 14, 28, 18, HI)                       # the top surface
    c.rect(3, 19, 28, 24, LIGHT)
    c.box(3, 21, 28, 25, MID)                       # the front face, darker
    c.rect(4, 22, 27, 24, GREY)
    c.disc(15, 16, 7, OUT)                          # the circular lid
    c.disc(15, 16, 6, HI)
    c.spiral(15, 16, 5, ORANGE)
    c.rect(4, 22, 7, 23, DARK)                      # a port on the front
    c.px(26, 24, DARK)


def pcengine(c):
    # a small white console with a HuCard standing out of its slot
    c.box(5, 14, 26, 24, HI)
    c.rect(6, 15, 25, 23, HI)
    c.rect(6, 21, 25, 23, LIGHT)                    # the low front
    c.box(10, 8, 18, 15, LIGHT)                     # the HuCard
    c.rect(11, 9, 17, 13, HI)
    c.rect(11, 11, 17, 12, MID)                     # the card's own label
    c.rect(9, 14, 19, 15, DARK)                     # the slot
    c.disc(23, 19, 1, RED)                          # a power light
    c.rect(7, 18, 16, 19, MID)                      # the controller ports


def atari2600(c):
    # the black console with the wood-grain front and a cartridge in the slot
    c.box(2, 15, 29, 25, DARK)
    c.rect(3, 16, 28, 24, DARK)
    c.rect(3, 16, 28, 17, MID)                      # top edge highlight
    c.rect(3, 21, 28, 24, BROWN)                    # the wood front
    for x in range(4, 28, 3):
        c.rect(x, 21, x, 24, BROWND)
    c.box(11, 8, 20, 16, DARK)                      # the cartridge
    c.rect(12, 9, 19, 14, MID)
    c.rect(13, 10, 18, 12, RED)                     # its label
    c.rect(4, 18, 6, 19, BLACK)                     # a switch


def c64(c):
    # the beige wedge ("breadbin"): a raised back, the dark keyboard on the low front
    c.box(3, 12, 28, 24, BEIGE)
    c.rect(4, 13, 27, 19, BEIGEL)                   # the raised back
    c.rect(4, 19, 27, 23, BEIGE)                    # the keyboard deck
    c.box(5, 17, 26, 23, DARK)                      # the keyboard recess
    for y in range(18, 23, 2):
        for x in range(6, 26, 2):
            c.px(x, y, BEIGE)
    c.rect(6, 13, 13, 14, BLUE)                     # the badge
    c.rect(15, 13, 21, 14, RED)


def zxspectrum(c):
    # the black wedge with the rainbow on the front edge
    c.box(2, 12, 29, 24, DARK)
    c.rect(3, 13, 28, 23, DARK)
    c.rect(3, 13, 28, 14, MID)                      # the top edge
    c.box(4, 16, 27, 22, BLACK)                     # the rubber-key well
    for y in range(17, 22, 2):
        for x in range(5, 27, 2):
            c.px(x, y, MID)
            c.px(x + 1, y, OUT)
    # the rainbow along the front, left to right
    for i, col in enumerate((RED, ORANGE, YELLOW, GREEN, BLUE)):
        c.rect(4 + i * 5, 22, 8 + i * 5, 23, col)


ICONS = {
    "gamegear": gamegear,
    "dreamcast": dreamcast,
    "pcengine": pcengine,
    "atari2600": atari2600,
    "c64": c64,
    "zxspectrum": zxspectrum,
}


def write_png(path, c):
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):
            raw.extend(c.p[y][x])

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xffffffff)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    outdir = os.path.normpath(sys.argv[1]) if len(sys.argv) > 1 else os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "ui", "assets", "rnds", "icons"))
    os.makedirs(outdir, exist_ok=True)
    for name, draw in ICONS.items():
        c = Canvas()
        draw(c)
        path = os.path.join(outdir, name + ".png")
        write_png(path, c)
        print("wrote " + path)


if __name__ == "__main__":
    main()
