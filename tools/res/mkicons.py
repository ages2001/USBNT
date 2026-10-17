#!/usr/bin/env python3
"""Draws the USBNT icons as 16-colour pixel art (the palette of NT 3.x / VGA)
and writes the .ico files of the programs, the small pictures of the
usbtree list (treeicons.bmp + treemask.bmp, 16x16 each, in a row) and the
autoscroll cursors of usbmon (as_*.cur, 32x32 monochrome).

  python3 mkicons.py [preview.png]
"""
import os, struct, sys
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
VGA = [(0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0), (0, 0, 128), (128, 0, 128), (0, 128, 128),
       (192, 192, 192), (128, 128, 128), (255, 0, 0), (0, 255, 0), (255, 255, 0), (0, 0, 255), (255, 0, 255),
       (0, 255, 255), (255, 255, 255)]
K, DR, DG, OL, NV, PU, TE, SI, GR, RE, LI, YE, BL, MA, CY, WH = range(16)
T = -1                                          # transparent


class Pic:
    def __init__(self, n):
        self.n = n
        self.p = [[T] * n for _ in range(n)]

    def px(self, x, y, c):
        if 0 <= x < self.n and 0 <= y < self.n:
            self.p[y][x] = c

    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.px(x, y, c)

    def frame(self, x0, y0, x1, y1, c):
        for x in range(x0, x1 + 1):
            self.px(x, y0, c); self.px(x, y1, c)
        for y in range(y0, y1 + 1):
            self.px(x0, y, c); self.px(x1, y, c)

    def box3d(self, x0, y0, x1, y1, face=SI, light=WH, dark=GR, edge=K):
        """a raised box: light top/left, dark bottom/right, black outline"""
        self.rect(x0, y0, x1, y1, face)
        self.frame(x0, y0, x1, y1, edge)
        for x in range(x0 + 1, x1):
            self.px(x, y0 + 1, light); self.px(x, y1 - 1, dark)
        for y in range(y0 + 1, y1):
            self.px(x0 + 1, y, light); self.px(x1 - 1, y, dark)

    def line(self, x0, y0, x1, y1, c):
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        e = dx + dy
        while True:
            self.px(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * e
            if e2 >= dy:
                e += dy; x0 += sx
            if e2 <= dx:
                e += dx; y0 += sy

    def disc(self, cx, cy, r, c, edge=None):
        for y in range(self.n):
            for x in range(self.n):
                d = (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2
                if d <= r * r:
                    self.px(x, y, edge if edge is not None and d > (r - 1) ** 2 else c)

    def fill(self, x, y, c):
        old = self.p[y][x]
        st = [(x, y)]
        while st:
            x, y = st.pop()
            if 0 <= x < self.n and 0 <= y < self.n and self.p[y][x] == old and old != c:
                self.p[y][x] = c
                st += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]

    def art(self, x0, y0, rows, keys):
        for j, row in enumerate(rows):
            for i, ch in enumerate(row):
                if ch in keys:
                    self.px(x0 + i, y0 + j, keys[ch])

    def outline(self, c=K):
        """a dark edge around every opaque area (the icon's silhouette)"""
        q = [r[:] for r in self.p]
        for y in range(self.n):
            for x in range(self.n):
                if self.p[y][x] == T:
                    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        xx, yy = x + dx, y + dy
                        if 0 <= xx < self.n and 0 <= yy < self.n and self.p[yy][xx] not in (T, c):
                            q[y][x] = c
                            break
        self.p = q


# ---------------------------------------------------------------- symbols

def trident(p, x, y, s, c):
    """the USB symbol, s = 1 (16 px) or 2 (32 px)"""
    if s == 2:
        rows = ["..........##..........",
                ".........####.........",
                "........######........",
                "..........##..........",
                "..........##......###.",
                "..##......##......###.",
                ".####.....##......###.",
                "######....##.......#..",
                ".####.....##.......#..",
                "..##......##.......#..",
                "...#......##......##..",
                "....#.....##.....##...",
                ".....##...##...##.....",
                ".......##.##.##.......",
                ".........####.........",
                "..........##..........",
                ".........####.........",
                "........######........",
                "........######........",
                ".........####........."]
    else:
        rows = ["....#....",
                "...###...",
                "....#..##",
                ".#..#..##",
                "###.#...#",
                ".#..#..#.",
                "..#.#.#..",
                "...###...",
                "....#....",
                "...###...",
                "...###..."]
    p.art(x, y, rows, {'#': c})


# ---------------------------------------------------------------- 32x32 program icons

def sm_tree():
    p = Pic(16)
    p.rect(0, 0, 15, 15, NV); p.frame(0, 0, 15, 15, K)
    p.line(1, 1, 14, 1, BL); p.line(1, 1, 1, 14, BL)
    trident(p, 3, 2, 1, WH)
    return p


def sm_mon():
    p = Pic(16)
    p.box3d(0, 0, 15, 11)
    p.rect(2, 2, 13, 9, TE)
    p.art(4, 3, ["...#..#.",
                 "#..#..#.",
                 ".#.#.#..",
                 "..###...",
                 "...#....",
                 "..###..."], {'#': WH})
    p.rect(6, 12, 9, 12, GR)
    p.box3d(2, 13, 13, 15)
    return p


def sm_inst():
    p = Pic(16)
    p.rect(1, 8, 14, 15, OL); p.frame(1, 8, 14, 15, K)
    p.line(2, 9, 13, 9, YE)
    p.rect(5, 0, 10, 3, SI); p.frame(5, 0, 10, 3, K)
    p.rect(4, 4, 11, 7, NV); p.frame(4, 4, 11, 7, K)
    p.rect(7, 8, 8, 10, K)
    return p


def sm_uninst():
    p = sm_inst()
    for i in range(0, 2):
        p.line(8 + i, 9, 14 + i, 15, RE); p.line(14 + i, 9, 8 + i, 15, RE)
    return p


def sm_eject():
    p = Pic(16)
    p.box3d(0, 1, 15, 14)
    p.art(3, 3, ["....#....",
                 "...###...",
                 "..#####..",
                 ".#######.",
                 "#########"], {'#': DG})
    p.rect(3, 10, 11, 11, DG)
    return p


def ico_tree():
    p = Pic(32)
    # a blue tile with the USB symbol, a white tree in the corner
    p.rect(2, 2, 29, 29, NV)
    p.frame(1, 1, 30, 30, K)
    for i in range(2, 30):
        p.px(i, 2, BL); p.px(2, i, BL)
    for i in range(3, 30):
        p.px(i, 29, K); p.px(29, i, K)
    trident(p, 5, 5, 2, WH)
    return p


def ico_mon():
    p = Pic(32)
    # a monitor: silver case, teal screen with the USB symbol, a stand
    p.box3d(2, 2, 29, 23)
    p.rect(5, 5, 26, 19, TE); p.frame(4, 4, 27, 20, GR)
    for x in range(5, 27):
        p.px(x, 5, CY)
    trident(p, 11, 7, 1, WH)
    p.px(24, 21, LI); p.px(25, 21, LI)
    p.rect(12, 24, 19, 25, GR); p.frame(11, 24, 20, 26, K)
    p.box3d(7, 26, 24, 29)
    return p


def ico_cam():
    p = Pic(32)
    # a round webcam on a foot
    p.disc(16, 13, 11.5, GR, K)
    p.disc(15, 12, 10, SI)
    p.disc(14, 10, 4, WH)
    p.disc(16, 13, 11.5, T) if False else None
    p.disc(16, 13, 7, K)
    p.disc(16, 13, 5.5, NV)
    p.disc(16, 13, 3.5, BL)
    p.disc(15, 12, 1.5, CY)
    p.px(14, 11, WH)
    p.px(25, 7, RE); p.px(26, 7, RE); p.px(25, 8, RE); p.px(26, 8, DR)
    p.rect(14, 24, 17, 26, GR); p.frame(14, 24, 17, 26, K)
    p.box3d(8, 26, 23, 29)
    p.outline()
    return p


def ico_inst():
    p = Pic(32)
    # an open box with a USB plug going in
    p.rect(4, 15, 27, 29, OL); p.frame(4, 15, 27, 29, K)
    p.rect(5, 16, 26, 17, YE)
    for x in range(5, 27, 1):
        p.px(x, 21, YE if x % 2 else OL)
    for i in range(5):
        p.line(4 - i, 15 - i, 9 - i, 15 - i, YE if i else OL)
        p.line(22 + i, 15 - i, 27 + i, 15 - i, YE if i else OL)
    p.line(0, 11, 5, 11, K); p.line(0, 11, 4, 15, K)
    p.line(26, 11, 31, 11, K); p.line(31, 11, 27, 15, K)
    # plug
    p.rect(12, 1, 19, 8, SI); p.frame(12, 1, 19, 8, K)
    p.rect(14, 3, 15, 4, K); p.rect(16, 3, 17, 4, K)
    p.rect(10, 9, 21, 14, NV); p.frame(10, 9, 21, 14, K)
    p.line(11, 10, 20, 10, BL)
    p.rect(14, 15, 17, 18, K)
    # green arrow
    p.art(23, 2, ["..#..",
                  ".###.",
                  "#####",
                  "..#..",
                  "..#.."], {'#': DG})
    return p


def ico_uninst():
    p = ico_inst()
    for i in range(-1, 2):
        p.line(18 + i, 18, 29 + i, 29, RE)
        p.line(29 + i, 18, 18 + i, 29, RE)
    return p


def ico_eject():
    p = Pic(32)
    p.box3d(3, 6, 28, 26)
    p.art(8, 9, ["........#........",
                 ".......###.......",
                 "......#####......",
                 ".....#######.....",
                 "....#########....",
                 "...###########...",
                 "..#############..",
                 ".###############.",
                 "#################"], {'#': DG})
    p.rect(8, 20, 24, 22, DG)
    return p


def ico_wheel():
    p = Pic(32)
    # a mouse with a wheel
    rows = ["......########......",
            "....############....",
            "...##############...",
            "..################..",
            ".##################.",
            ".##################.",
            "####################",
            "####################",
            "####################",
            "####################",
            "####################",
            "####################",
            "####################",
            "####################",
            ".##################.",
            ".##################.",
            "..################..",
            "...##############...",
            "....############....",
            "......########......"]
    p.art(6, 6, rows, {'#': SI})
    p.line(16, 6, 16, 14, GR); p.line(7, 14, 25, 14, GR)
    p.rect(15, 8, 17, 12, K); p.px(16, 9, WH); p.px(16, 11, WH)
    p.art(8, 8, ["##", "#."], {'#': WH})
    p.outline()
    p.line(16, 0, 16, 5, K); p.line(16, 0, 20, 0, K)
    return p


# ---------------------------------------------------------------- 16x16 list pictures (usbtree)

def sm_from(big):
    """16x16 version of a 32x32 icon: every 2x2 block, the most frequent colour"""
    p = Pic(16)
    for y in range(16):
        for x in range(16):
            cs = [big.p[2 * y + j][2 * x + i] for j in (0, 1) for i in (0, 1)]
            op = [c for c in cs if c != T]
            if len(op) < 2:
                continue
            p.p[y][x] = max(set(op), key=op.count)
    return p


def g_chip(c=K, pins=SI, mark=None):
    p = Pic(16)
    p.rect(3, 3, 12, 12, c)
    for i in range(4, 12, 2):
        p.px(i, 1, pins); p.px(i, 2, pins); p.px(i, 13, pins); p.px(i, 14, pins)
        p.px(1, i, pins); p.px(2, i, pins); p.px(13, i, pins); p.px(14, i, pins)
    p.frame(3, 3, 12, 12, GR)
    if mark is not None:
        p.rect(6, 6, 9, 9, mark)
    return p


def g_hc():
    p = g_chip(K, SI)
    trident(p, 4, 3, 1, WH) if False else None
    p.art(4, 4, ["...#...",
                 "..###..",
                 "#..#..#",
                 "#..#..#",
                 ".#.#.#.",
                 "..###..",
                 "...#...",
                 "..###.."], {'#': CY})
    return p


def g_hub():
    p = Pic(16)
    p.box3d(0, 4, 15, 11)
    for x in (2, 6, 10):
        p.rect(x, 7, x + 3, 8, K)
    p.px(13, 6, LI)
    p.line(7, 0, 7, 3, K); p.line(8, 0, 8, 3, GR)
    return p


def g_stick():
    p = Pic(16)
    p.rect(1, 5, 11, 10, NV); p.frame(0, 4, 11, 11, K)
    p.line(1, 5, 10, 5, BL)
    p.rect(12, 5, 15, 10, SI); p.frame(11, 4, 15, 11, K)
    p.px(13, 6, K); p.px(13, 9, K)
    p.px(3, 7, LI)
    return p


def g_disk():
    p = Pic(16)
    p.box3d(0, 4, 15, 12)
    p.line(2, 9, 9, 9, GR); p.line(2, 10, 9, 10, WH)
    p.px(12, 9, LI); p.px(13, 9, LI)
    return p


def g_kbd():
    p = Pic(16)
    p.box3d(0, 4, 15, 12)
    for y in (6, 8):
        for x in range(2, 14, 2):
            p.px(x, y, K)
    p.line(4, 10, 11, 10, K)
    return p


def g_mouse():
    p = Pic(16)
    p.art(3, 2, ["...####...",
                 ".########.",
                 "##########",
                 "##########",
                 "##########",
                 "##########",
                 "##########",
                 "##########",
                 ".########.",
                 "..######..",
                 "...####..."], {'#': SI})
    p.line(8, 2, 8, 6, GR); p.line(3, 6, 12, 6, GR)
    p.px(8, 3, K); p.px(8, 4, K)
    p.outline()
    p.line(8, 0, 8, 1, K)
    return p


def g_tablet():
    p = Pic(16)
    p.box3d(0, 3, 13, 14, face=GR, light=SI, dark=K)
    p.rect(2, 5, 11, 12, K)
    p.line(15, 1, 8, 8, NV); p.line(14, 1, 8, 7, BL)
    p.px(8, 8, WH)
    return p


def g_cam():
    p = Pic(16)
    p.disc(8, 7, 6.5, SI, K)
    p.disc(8, 7, 3.6, K)
    p.disc(8, 7, 2.5, BL)
    p.px(7, 6, CY)
    p.px(13, 2, RE)
    p.rect(7, 13, 8, 13, K)
    p.rect(4, 14, 11, 15, GR); p.frame(4, 14, 11, 15, K)
    return p


def g_audio():
    p = Pic(16)
    p.rect(1, 5, 4, 10, SI); p.frame(1, 5, 4, 10, K)
    p.art(4, 1, ["...#",
                 "..##",
                 ".###",
                 "####",
                 "####",
                 "####",
                 "####",
                 "####",
                 "####",
                 "####",
                 ".###",
                 "..##",
                 "...#"], {'#': GR})
    p.line(5, 4, 7, 2, K); p.line(5, 11, 7, 13, K); p.line(7, 2, 7, 13, K)
    p.px(10, 6, NV); p.px(11, 7, NV); p.px(11, 8, NV); p.px(10, 9, NV)
    p.px(12, 4, NV); p.px(13, 5, NV); p.line(14, 6, 14, 9, NV); p.px(13, 10, NV); p.px(12, 11, NV)
    return p


def g_midi():
    p = Pic(16)
    p.rect(0, 3, 15, 13, WH); p.frame(0, 3, 15, 13, K)
    for x in (2, 4, 6, 8, 10, 12, 14):
        p.line(x, 4, x, 12, GR)
    for x in (1, 5, 7, 11, 13):
        p.rect(x, 4, x + 1, 8, K)
    return p


def g_sd():
    p = Pic(16)
    p.art(2, 0, ["########...",
                 "#########..",
                 "##########.",
                 "###########",
                 "###########",
                 "###########",
                 "###########",
                 "###########",
                 "###########",
                 "###########",
                 "###########",
                 "###########",
                 "###########",
                 "###########",
                 "###########",
                 "###########"], {'#': NV})
    for x in range(3, 11, 2):
        p.rect(x, 1, x, 3, YE)
    p.rect(4, 7, 11, 12, WH)
    p.art(5, 8, ["#.##.",
                 ".##.#",
                 "#.##."], {'#': NV})
    p.outline()
    return p


def g_reader():
    p = Pic(16)
    p.box3d(0, 7, 15, 14)
    p.rect(3, 8, 12, 9, K)
    # card going in
    p.rect(5, 1, 10, 8, NV); p.frame(5, 1, 10, 8, K)
    p.px(6, 2, YE); p.px(8, 2, YE)
    p.px(13, 12, LI)
    return p


def g_port():
    p = Pic(16)
    p.rect(2, 5, 13, 10, GR); p.frame(1, 4, 14, 11, K)
    p.rect(3, 6, 12, 7, WH)
    p.rect(3, 8, 12, 9, K)
    return p


def g_touchpad():
    p = Pic(16)
    p.box3d(0, 1, 15, 14, face=GR, light=SI, dark=K)
    p.rect(2, 3, 13, 9, K)
    p.line(2, 11, 7, 11, SI); p.line(9, 11, 13, 11, SI)
    p.line(2, 12, 7, 12, WH); p.line(9, 12, 13, 12, WH)
    p.px(6, 5, CY); p.px(9, 6, CY)
    return p


def g_i2c():
    return g_chip(DG, SI, LI)


def g_plug():
    p = Pic(16)
    p.rect(5, 0, 10, 4, SI); p.frame(5, 0, 10, 4, K)
    p.rect(6, 1, 6, 2, K); p.rect(9, 1, 9, 2, K)
    p.rect(3, 5, 12, 11, NV); p.frame(3, 5, 12, 11, K)
    p.px(6, 7, WH); p.px(7, 7, WH); p.px(8, 8, WH)
    p.rect(6, 12, 9, 15, K)
    return p


def g_nvme():
    """an M.2 drive: green board, two chips, the key notch at the right"""
    p = Pic(16)
    p.rect(0, 5, 14, 10, DG); p.frame(0, 5, 14, 10, K)
    p.rect(2, 6, 5, 9, K); p.rect(7, 6, 10, 9, K)
    p.px(3, 7, GR); p.px(8, 7, GR)
    for x in range(12, 15):
        p.px(x, 6, YE); p.px(x, 9, YE)
    p.px(14, 7, T); p.px(14, 8, T)
    p.px(1, 4, T)
    return p


def g_tbt():
    """a lightning bolt over a USB-C plug"""
    p = Pic(16)
    p.rect(2, 11, 13, 14, SI); p.frame(1, 10, 14, 15, K)
    p.line(4, 12, 11, 12, K)
    p.art(4, 0, ["....###",
                 "...###.",
                 "..###..",
                 ".######",
                 "....##.",
                 "...##..",
                 "..##...",
                 ".##....",
                 "##....."], {'#': YE})
    p.outline(K)
    return p


GLYPHS = [g_hc, g_hub, g_stick, g_disk, g_kbd, g_mouse, g_tablet, g_cam, g_audio, g_midi, g_sd, g_reader,
          g_port, g_touchpad, g_i2c, g_plug, g_nvme, g_tbt]
ICONS = [('usbtree', [(ico_tree, sm_tree)]), ('usbmon', [(ico_mon, sm_mon)]), ('usbcam', [(ico_cam, lambda: g_cam())]),
         ('usbinst', [(ico_inst, sm_inst), (ico_uninst, sm_uninst)]), ('usbeject', [(ico_eject, sm_eject)]),
         ('wheeltst', [(ico_wheel, lambda: g_mouse())])]


# ---------------------------------------------------------------- autoscroll cursors (usbmon)

def tri(p, a, b, c, col):
    """filled triangle"""
    def side(p1, p2, q):
        return (p2[0] - p1[0]) * (q[1] - p1[1]) - (p2[1] - p1[1]) * (q[0] - p1[0])
    for y in range(p.n):
        for x in range(p.n):
            q = (x + 0.5, y + 0.5)
            d1, d2, d3 = side(a, b, q), side(b, c, q), side(c, a, q)
            if (d1 >= 0 and d2 >= 0 and d3 >= 0) or (d1 <= 0 and d2 <= 0 and d3 <= 0):
                p.px(x, y, col)


C = 16.0                                        # centre of the 32 x 32 cursor

def arrow(p, dx, dy, near, far, half):
    """a triangle pointing away from the centre in direction (dx, dy)"""
    l = (dx * dx + dy * dy) ** 0.5
    dx, dy = dx / l, dy / l
    bx, by = C + dx * near, C + dy * near
    tx, ty = C + dx * far, C + dy * far
    tri(p, (tx, ty), (bx - dy * half, by + dx * half), (bx + dy * half, by - dx * half), K)


def scroll_cursor(dirs, big=False):
    """centre dot and an arrow for every direction; black with a white edge"""
    p = Pic(32)
    p.disc(C, C, 2.6, K)
    for dx, dy in dirs:
        if big:
            arrow(p, dx, dy, 5, 14.5, 6.5)
        else:
            arrow(p, dx, dy, 6, 13.5, 5)
    p.outline(WH)
    return p


N, S, E, W_ = (0, -1), (0, 1), (1, 0), (-1, 0)
CURSORS = [('as_all', [N, S, E, W_], False), ('as_ns', [N, S], False), ('as_ew', [E, W_], False),
           ('as_n', [N], True), ('as_s', [S], True), ('as_e', [E], True), ('as_w', [W_], True),
           ('as_ne', [(1, -1)], True), ('as_nw', [(-1, -1)], True), ('as_se', [(1, 1)], True),
           ('as_sw', [(-1, 1)], True)]


def cur_bytes(p, hx, hy):
    """a monochrome .cur: black = 0/0, white = 0/1 (XOR / AND), transparent = 1 in the AND mask"""
    n = p.n
    xor, andm = bytearray(), bytearray()
    for y in range(n - 1, -1, -1):
        xb = ab = 0
        for x in range(n):
            c = p.p[y][x]
            xb = (xb << 1) | (1 if c == WH else 0)
            ab = (ab << 1) | (1 if c == T else 0)
            if x % 8 == 7:
                xor.append(xb); andm.append(ab); xb = ab = 0
    hdr = struct.pack('<IiiHHIIiiII', 40, n, n * 2, 1, 1, 0, len(xor) + len(andm), 0, 0, 2, 0)
    pal = struct.pack('BBBBBBBB', 0, 0, 0, 0, 255, 255, 255, 0)
    d = hdr + pal + bytes(xor) + bytes(andm)
    return struct.pack('<HHH', 0, 2, 1) + struct.pack('<BBBBHHII', n, n, 2, 0, hx, hy, len(d), 22) + d


# ---------------------------------------------------------------- files

def bmp4(p):
    """bottom-up 4-bit pixels and a 1-bit AND mask (transparent = 1)"""
    n = p.n
    xor = bytearray()
    for y in range(n - 1, -1, -1):
        row = [c if c != T else 0 for c in p.p[y]]
        b = bytearray((row[i] << 4) | row[i + 1] for i in range(0, n, 2))
        while len(b) % 4:
            b.append(0)
        xor += b
    andm = bytearray()
    for y in range(n - 1, -1, -1):
        bits = 0
        out = bytearray()
        for x in range(n):
            bits = (bits << 1) | (1 if p.p[y][x] == T else 0)
            if x % 8 == 7:
                out.append(bits); bits = 0
        while len(out) % 4:
            out.append(0)
        andm += out
    return bytes(xor), bytes(andm)


def ico_bytes(pics):
    """an .ico with the images (32x32 and 16x16, 16 colours each)"""
    imgs = []
    for p in pics:
        xor, andm = bmp4(p)
        hdr = struct.pack('<IiiHHIIiiII', 40, p.n, p.n * 2, 1, 4, 0, len(xor) + len(andm), 0, 0, 16, 0)
        pal = b''.join(struct.pack('BBBB', b, g, r, 0) for r, g, b in VGA)
        imgs.append(hdr + pal + xor + andm)
    out = struct.pack('<HHH', 0, 1, len(pics))
    off = 6 + 16 * len(pics)
    for p, d in zip(pics, imgs):
        out += struct.pack('<BBBBHHII', p.n, p.n, 16, 0, 1, 4, len(d), off)
        off += len(d)
    return out + b''.join(imgs)


def strip_bmps(glyphs):
    """treeicons.bmp (colours, transparent = black) and treemask.bmp (white where transparent)"""
    w, h = 16 * len(glyphs), 16
    col = Image.new('P', (w, h), 0)
    col.putpalette([v for c in VGA for v in c])
    mask = Image.new('1', (w, h), 0)
    for k, g in enumerate(glyphs):
        for y in range(16):
            for x in range(16):
                c = g.p[y][x]
                if c == T:
                    mask.putpixel((16 * k + x, y), 1)
                else:
                    col.putpixel((16 * k + x, y), c)
    return col, mask


def write_bmp4(img, path):
    """4-bit BMP (Pillow writes 8 bits for palette images; NT 3.1 takes 4)"""
    w, h = img.size
    stride = ((w + 1) // 2 + 3) & ~3
    data = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(0, w, 2):
            a = img.getpixel((x, y)); b = img.getpixel((x + 1, y)) if x + 1 < w else 0
            row.append((a << 4) | b)
        while len(row) < stride:
            row.append(0)
        data += row
    pal = b''.join(struct.pack('BBBB', b, g, r, 0) for r, g, b in VGA)
    info = struct.pack('<IiiHHIIiiII', 40, w, h, 1, 4, 0, len(data), 0, 0, 16, 0)
    off = 14 + len(info) + len(pal)
    open(path, 'wb').write(struct.pack('<2sIHHI', b'BM', off + len(data), 0, 0, off) + info + pal + data)


def render(p, scale):
    im = Image.new('RGB', (p.n * scale, p.n * scale), (0, 128, 128))
    d = ImageDraw.Draw(im)
    for y in range(p.n):
        for x in range(p.n):
            if p.p[y][x] != T:
                d.rectangle([x * scale, y * scale, x * scale + scale - 1, y * scale + scale - 1], fill=VGA[p.p[y][x]])
    return im


def main():
    previews = []
    for name, makers in ICONS:
        pics = []
        for i, (mk, mks) in enumerate(makers):
            big = mk()
            small = mks()
            pics += [big, small]
            previews.append((big, small))
        # one .ico per icon; the second icon of usbinst is its own file
        for i in range(len(makers)):
            fn = name + ('' if i == 0 else str(i + 1)) + '.ico'
            open(os.path.join(HERE, fn), 'wb').write(ico_bytes(pics[2 * i:2 * i + 2]))
    curs = []
    for name, dirs, big in CURSORS:
        c = scroll_cursor(dirs, big)
        open(os.path.join(HERE, name + '.cur'), 'wb').write(cur_bytes(c, 16, 16))
        curs.append(c)
    gl = [g() for g in GLYPHS]
    col, mask = strip_bmps(gl)
    write_bmp4(col, os.path.join(HERE, 'treeicons.bmp'))
    mask.save(os.path.join(HERE, 'treemask.bmp'))
    if len(sys.argv) > 1:
        sc = 6
        W = max(len(previews) * 36 * sc // 1, len(gl) * 18 * sc)
        im = Image.new('RGB', (W, 36 * sc + 18 * sc + 16 * 3 + 40 + 34 * 4), (192, 192, 192))
        for i, (b, s) in enumerate(previews):
            im.paste(render(b, sc), (i * 36 * sc, 0))
            im.paste(render(s, 3), (i * 36 * sc + 33 * sc, 0))
            im.paste(render(b, 1), (i * 36 * sc + 33 * sc, 50))
            im.paste(render(s, 1), (i * 36 * sc + 33 * sc, 90))
        for i, g in enumerate(gl):
            im.paste(render(g, sc), (i * 18 * sc, 36 * sc))
            im.paste(render(g, 1), (i * 18 * sc, 36 * sc + 17 * sc))
        for i, c in enumerate(curs):
            im.paste(render(c, 4), (i * 34 * 4, 36 * sc + 18 * sc + 40))
        im.save(sys.argv[1])


if __name__ == '__main__':
    main()
