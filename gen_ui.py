#!/usr/bin/env python3
"""Run by build.sh before the build. Generates params.json, layout.conf, vst.json and the skin images for CRATE DIGGA (JJ DA PLUG).
Look: black sticker with white hand-drawn lines, gold, own dog and cat characters.
All big lettering is turned into outlines from Bebas Neue (SIL Open Font License, see OFL-*.txt), so it looks the same everywhere."""
import json, os, shutil
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen

OUT = os.path.dirname(os.path.abspath(__file__))
IMG = OUT + "/images"
os.makedirs(IMG, exist_ok=True)
if os.path.exists(OUT + "/logo.png") and os.path.abspath(OUT + "/logo.png") != os.path.abspath(IMG + "/logo.png"):
    shutil.copy(OUT + "/logo.png", IMG + "/logo.png")
ROWS = 7
AMBER, AMBER2, SILVER, GREY, DIM = "#ffb52e", "#c98410", "#e9e7e0", "#9a968c", "#6b675e"
KEYS = "qwertyuiopasdfghjklzxcvbnm"
EKEYS = "qwertyuiopasdfghjklzxcvbnm0123456789@._-"
QUICK = ["jazz", "funk", "soul", "film", "blues", "ambient", "cartoon", "drums"]
VERSION_TXT = "1.6"

# ------------------------------------------------------------------ params (v1.0 order first, new ones appended at the end)
P = []
def opt(key, name, options, default=0): P.append({"key": key, "name": name, "options": options, "default": default})
def ro(key, name): P.append({"key": key, "name": name, "type": "readout", "display": "string"})
def trig(key, name): P.append({"key": key, "name": name, "type": "trigger", "momentary": True})
opt("cat", "Type", ["music", "words", "sounds"])
opt("shortonly", "Short only", ["off", "on"], 1)
for k, n in [("q_txt", "Search word"), ("hdr_txt", "Status"), ("status_txt", "Search status"), ("note_txt", "Notice"), ("rs_info", "Result info"),
             ("r_page_txt", "Results page"), ("c_page_txt", "Crate page"), ("pv_txt", "Playing"), ("to_pad_txt", "To pad"), ("pad_info_txt", "Pad info"),
             ("learn_txt", "Learn status"), ("net_txt", "Connection"), ("dir_txt", "Folder")]: ro(k, n)
for i in range(1, ROWS + 1):
    ro("r%d" % i, "Result %d" % i); trig("rt%d" % i, "Result %d tap" % i)
    ro("c%d" % i, "Crate %d" % i); trig("ct%d" % i, "Crate %d tap" % i)
for i in range(1, 17):
    ro("pn%d" % i, "Pad %d name" % i); trig("pd%d" % i, "Pad %d" % i)
for i in range(26): trig("kc%d" % i, "Key %s" % KEYS[i])
for i in range(1, 9): trig("pq%d" % i, "Quick %s" % QUICK[i - 1])
for t, n in [("k_space", "Space"), ("k_back", "Backspace"), ("k_clear", "Clear"), ("do_search", "Search"), ("r_prev", "Previous"), ("r_next", "Next"),
             ("pv_play", "Play or stop"), ("keep", "Keep"), ("to_pad", "To pad"), ("c_prev", "Crate previous"), ("c_next", "Crate next"), ("c_del", "Crate remove"),
             ("pad_clear", "Clear pad"), ("learn_pad", "Learn pad"), ("learn_base", "Learn all pads"), ("net_test", "Test connection")]: trig(t, n)
# ---- new in 1.1
for k, n in [("email_txt", "E-mail"), ("acc_txt", "Account status"), ("last_txt", "Last MIDI note"), ("att_txt", "Attack value"), ("dec_txt", "Decay value"),
             ("rel_txt", "Release value"), ("grp_txt", "Mute group value"), ("mode_txt", "Mode value")]: ro(k, n)
for i in range(40): trig("ec%d" % i, "E-mail key %s" % EKEYS[i])
for t, n in [("e_back", "E-mail backspace"), ("e_clear", "E-mail clear"), ("register", "Register account"), ("acc_forget", "Forget account"),
             ("att_m", "Attack less"), ("att_p", "Attack more"), ("dec_m", "Decay less"), ("dec_p", "Decay more"), ("rel_m", "Release less"),
             ("rel_p", "Release more"), ("grp_m", "Group less"), ("grp_p", "Group more"), ("mode_t", "Mode switch")]: trig(t, n)
# ---- new in 1.2: the turntable (appended at the end)
P.append({"key": "pitch", "name": "Pitch", "min": 0, "max": 1, "default": 0.5})
P.append({"key": "jog", "name": "Scratch", "min": 0, "max": 1, "default": 0.5})
P.append({"key": "plat", "name": "Platter", "min": 0, "max": 1, "default": 0})
opt("prange", "Pitch range", ["8%", "16%", "50%", "OCT"], 3)
opt("klock", "Key lock", ["KEY OFF", "KEY ON"], 0)
ro("pitch_txt", "Pitch value")
trig("deck_cue", "Cue")
# ---- new in 1.6: the sample editor (waveform, start/end, ADSR) and STOP ALL
NWV, NEV = 80, 48
for k, n, dflt in [("ed_start", "Start", 0), ("ed_end", "End", 1), ("ed_a", "Attack", 0), ("ed_d", "Decay", 0.3), ("ed_s", "Sustain", 1), ("ed_r", "Release", 0.15)]:
    P.append({"key": k, "name": n, "min": 0, "max": 1, "default": dflt})
for k, n in [("ed_a_txt", "Attack value"), ("ed_d_txt", "Decay value"), ("ed_s_txt", "Sustain value"), ("ed_r_txt", "Release value"), ("ed_pos_txt", "Start and end"), ("ed_pad_txt", "Edited pad")]: ro(k, n)
trig("ed_play", "Play pad"); trig("stop_all", "Stop all")
for i in range(NWV): P.append({"key": "wv%d" % i, "name": "Wave %d" % i, "min": 0, "max": 1, "default": 0})
for i in range(NEV): P.append({"key": "ev%d" % i, "name": "Envelope %d" % i, "min": 0, "max": 1, "default": 0})
json.dump({"name": "Crate Digga", "params": P}, open(OUT + "/params.json", "w"), indent=1)

# ================================================================== look: black sticker, white hand-drawn lines, gold, own dog and cat
import math, random
from PIL import Image, ImageDraw
INK, BLK, GOLD, PINK, DIM2 = "#f4f1ea", "#0e0e0f", "#ffb52e", "#ff6b8a", "#3a3834"

# ------------------------------------------------------------------ lettering as outlines
FONTS = {}
def font(name):
    if name not in FONTS:
        t = TTFont(OUT + "/%s.ttf" % name)
        FONTS[name] = (t, t.getGlyphSet(), t.getBestCmap(), t["head"].unitsPerEm, t["hmtx"])
    return FONTS[name]
_gcache = {}
def glyph_d(fn, g):
    k = (fn, g)
    if k not in _gcache:
        t, gs, cm, upm, hm = font(fn)
        pen = SVGPathPen(gs); gs[g].draw(pen); _gcache[k] = pen.getCommands()
    return _gcache[k]
def text_w(s, size, fn="BebasNeue", spacing=0.0):
    t, gs, cm, upm, hm = font(fn)
    return sum(hm[cm.get(ord(c), cm[ord("?")])][0] * size / upm + spacing for c in s) - spacing
def tx(s, x, y, size, fill, anchor="start", fn="BebasNeue", spacing=0.0):
    t, gs, cm, upm, hm = font(fn)
    w = text_w(s, size, fn, spacing)
    cx = x - (w / 2 if anchor == "middle" else w if anchor == "end" else 0)
    out = []; sc = size / upm
    for c in s:
        g = cm.get(ord(c), cm[ord("?")]); d = glyph_d(fn, g)
        if d: out.append('<path transform="translate(%.2f %.2f) scale(%.5f %.5f)" d="%s"/>' % (cx, y, sc, -sc, d))
        cx += hm[g][0] * sc + spacing
    return '<g fill="%s">%s</g>' % (fill, "".join(out))
def txc(s, x, y, size, fill, anchor="middle", fn="BebasNeue", spacing=0.0):
    return tx(s, x, y + size * 0.35, size, fill, anchor, fn, spacing)
def svg(name, w, h, body, defs=""):
    open("%s/%s.svg" % (IMG, name), "w").write('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d"><defs>%s</defs>%s</svg>' % (w, h, w, h, defs, body))

# ------------------------------------------------------------------ hand-drawn helpers (wobbly lines, deterministic)
RND = random.Random(11)
def smooth(pts, closed=True):
    n = len(pts)
    if n < 3: return ""
    d = "M%.1f %.1f" % pts[0]
    rng = range(n) if closed else range(n - 1)
    for i in rng:
        p0, p1, p2, p3 = pts[(i - 1) % n], pts[i], pts[(i + 1) % n], pts[(i + 2) % n]
        if not closed:
            p0 = pts[max(i - 1, 0)]; p3 = pts[min(i + 2, n - 1)]
        c1 = (p1[0] + (p2[0] - p0[0]) / 6, p1[1] + (p2[1] - p0[1]) / 6)
        c2 = (p2[0] - (p3[0] - p1[0]) / 6, p2[1] - (p3[1] - p1[1]) / 6)
        d += " C%.1f %.1f %.1f %.1f %.1f %.1f" % (c1 + c2 + p2)
    return d + (" Z" if closed else "")
def jit(pts, j): return [(x + RND.uniform(-j, j), y + RND.uniform(-j, j)) for x, y in pts]
def rr_pts(x, y, w, h, r, step=22):
    pts = []
    def edge(x1, y1, x2, y2):
        L = math.hypot(x2 - x1, y2 - y1); k = max(1, int(L / step))
        for i in range(k): pts.append((x1 + (x2 - x1) * i / k, y1 + (y2 - y1) * i / k))
    def arc(cx, cy, a0):
        for i in range(4): a = math.radians(a0 + 90 * i / 4); pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    edge(x + r, y, x + w - r, y); arc(x + w - r, y + r, -90)
    edge(x + w, y + r, x + w, y + h - r); arc(x + w - r, y + h - r, 0)
    edge(x + w - r, y + h, x + r, y + h); arc(x + r, y + h - r, 90)
    edge(x, y + h - r, x, y + r); arc(x + r, y + r, 180)
    return pts
def sk_rect(x, y, w, h, r, fill, stroke, sw=3, j=1.3, twice=True):
    """a rounded box drawn by hand: wobbly outline, optionally a second slightly different pass"""
    s = '<path d="%s" fill="%s" stroke="%s" stroke-width="%s" stroke-linejoin="round"/>' % (smooth(jit(rr_pts(x, y, w, h, r), j)), fill, stroke, sw)
    if twice: s += '<path d="%s" fill="none" stroke="%s" stroke-width="%s" opacity=".55" stroke-linejoin="round"/>' % (smooth(jit(rr_pts(x, y, w, h, r), j * 1.5)), stroke, max(1, sw - 1.5))
    return s
def sk_line(x1, y1, x2, y2, stroke=INK, sw=3, j=1.2, bend=2.5):
    mx, my = (x1 + x2) / 2 + RND.uniform(-bend, bend), (y1 + y2) / 2 + RND.uniform(-bend, bend)
    return '<path d="M%.1f %.1f Q%.1f %.1f %.1f %.1f" fill="none" stroke="%s" stroke-width="%s" stroke-linecap="round"/>' % (x1 + RND.uniform(-j, j), y1 + RND.uniform(-j, j), mx, my, x2 + RND.uniform(-j, j), y2 + RND.uniform(-j, j), stroke, sw)
def sk_ell(cx, cy, rx, ry, fill, stroke=INK, sw=3, j=1.2, rot=0):
    pts = [(cx + rx * math.cos(2 * math.pi * i / 14), cy + ry * math.sin(2 * math.pi * i / 14)) for i in range(14)]
    return '<path d="%s" fill="%s" stroke="%s" stroke-width="%s" stroke-linejoin="round" transform="rotate(%s %s %s)"/>' % (smooth(jit(pts, j)), fill, stroke, sw, rot, cx, cy)
def sk_poly(pts, fill, stroke=INK, sw=3, j=1.2):
    return '<path d="%s" fill="%s" stroke="%s" stroke-width="%s" stroke-linejoin="round"/>' % (smooth(jit(pts, j)), fill, stroke, sw)

# ------------------------------------------------------------------ the characters (my own): a dog with headphones and a cat with sunglasses
def dog(cx, cy, s=1.0, tongue=True):
    g = ""
    g += sk_ell(cx - 44 * s, cy + 6 * s, 15 * s, 30 * s, INK, INK, 3, 1, 18)            # floppy ears
    g += sk_ell(cx + 44 * s, cy + 6 * s, 15 * s, 30 * s, INK, INK, 3, 1, -18)
    g += sk_ell(cx, cy, 40 * s, 36 * s, BLK, INK, 3.5, 1.4)                              # head
    g += '<path d="M%.1f %.1f Q%.1f %.1f %.1f %.1f" fill="none" stroke="%s" stroke-width="%.1f" stroke-linecap="round"/>' % (cx - 38 * s, cy - 6 * s, cx, cy - 66 * s, cx + 38 * s, cy - 6 * s, GOLD, 7 * s)   # headphone band
    g += sk_ell(cx - 41 * s, cy - 2 * s, 8 * s, 13 * s, GOLD, INK, 2.5, .8) + sk_ell(cx + 41 * s, cy - 2 * s, 8 * s, 13 * s, GOLD, INK, 2.5, .8)
    g += sk_ell(cx, cy + 12 * s, 20 * s, 15 * s, INK, INK, 2.5, 1)                        # snout
    g += sk_ell(cx, cy + 4 * s, 7.5 * s, 5.5 * s, BLK, BLK, 1, .6)                        # nose
    g += '<circle cx="%.1f" cy="%.1f" r="%.1f" fill="#fff"/>' % (cx - 2 * s, cy + 2.5 * s, 1.6 * s)
    g += sk_ell(cx - 16 * s, cy - 10 * s, 6.5 * s, 7.5 * s, INK, INK, 2, .6) + sk_ell(cx + 16 * s, cy - 10 * s, 6.5 * s, 7.5 * s, INK, INK, 2, .6)   # eyes
    g += '<circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s"/><circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s"/>' % (cx - 15 * s, cy - 9 * s, 3.2 * s, BLK, cx + 17 * s, cy - 9 * s, 3.2 * s, BLK)
    g += '<path d="M%.1f %.1f Q%.1f %.1f %.1f %.1f M%.1f %.1f Q%.1f %.1f %.1f %.1f" fill="none" stroke="%s" stroke-width="%.1f" stroke-linecap="round"/>' % (cx, cy + 9 * s, cx - 5 * s, cy + 20 * s, cx - 12 * s, cy + 17 * s, cx, cy + 9 * s, cx + 5 * s, cy + 20 * s, cx + 12 * s, cy + 17 * s, BLK, 2.4 * s)
    if tongue: g += sk_ell(cx, cy + 25 * s, 6 * s, 8 * s, PINK, BLK, 2, .6)
    return g
def cat(cx, cy, s=1.0, shades=True):
    g = ""
    g += sk_poly([(cx - 40 * s, cy - 10 * s), (cx - 36 * s, cy - 50 * s), (cx - 10 * s, cy - 30 * s)], BLK, INK, 3.5, 1) + sk_poly([(cx - 33 * s, cy - 20 * s), (cx - 32 * s, cy - 40 * s), (cx - 19 * s, cy - 29 * s)], PINK, PINK, 1, .6)
    g += sk_poly([(cx + 40 * s, cy - 10 * s), (cx + 36 * s, cy - 50 * s), (cx + 10 * s, cy - 30 * s)], BLK, INK, 3.5, 1) + sk_poly([(cx + 33 * s, cy - 20 * s), (cx + 32 * s, cy - 40 * s), (cx + 19 * s, cy - 29 * s)], PINK, PINK, 1, .6)
    g += sk_ell(cx, cy, 44 * s, 35 * s, BLK, INK, 3.5, 1.4)
    for k in (-1, 0, 1): g += sk_line(cx + k * 9 * s, cy - 33 * s, cx + k * 9 * s, cy - 22 * s, INK, 2.5, .5, 1)   # stripes
    if shades:
        g += sk_rect(cx - 36 * s, cy - 14 * s, 31 * s, 19 * s, 7 * s, "#000", INK, 3, .8, False) + sk_rect(cx + 5 * s, cy - 14 * s, 31 * s, 19 * s, 7 * s, "#000", INK, 3, .8, False)
        g += sk_line(cx - 6 * s, cy - 8 * s, cx + 6 * s, cy - 8 * s, INK, 3, .3, .5)
        g += sk_line(cx - 30 * s, cy - 10 * s, cx - 20 * s, cy - 10 * s, "#999", 2, .2, .3) + sk_line(cx + 11 * s, cy - 10 * s, cx + 21 * s, cy - 10 * s, "#999", 2, .2, .3)
    else:
        for sx in (-1, 1): g += '<path d="M%.1f %.1f Q%.1f %.1f %.1f %.1f" fill="none" stroke="%s" stroke-width="%.1f" stroke-linecap="round"/>' % (cx + sx * 24 * s - 8 * s, cy - 5 * s, cx + sx * 24 * s, cy - 15 * s, cx + sx * 24 * s + 8 * s, cy - 5 * s, INK, 3 * s)
    g += sk_poly([(cx - 5 * s, cy + 8 * s), (cx + 5 * s, cy + 8 * s), (cx, cy + 14 * s)], PINK, PINK, 1, .4)
    g += '<path d="M%.1f %.1f Q%.1f %.1f %.1f %.1f M%.1f %.1f Q%.1f %.1f %.1f %.1f" fill="none" stroke="%s" stroke-width="%.1f" stroke-linecap="round"/>' % (cx, cy + 14 * s, cx - 5 * s, cy + 22 * s, cx - 11 * s, cy + 18 * s, cx, cy + 14 * s, cx + 5 * s, cy + 22 * s, cx + 11 * s, cy + 18 * s, INK, 2.4 * s)
    for sx in (-1, 1):
        for dy in (-3, 5): g += sk_line(cx + sx * 20 * s, cy + (14 + dy / 2) * s, cx + sx * 58 * s, cy + (12 + dy * 2) * s, INK, 2, .4, 1)
    return g
def paw(x, y, s=1.0, fill=DIM2):
    g = sk_ell(x, y + 5 * s, 9 * s, 7 * s, fill, fill, 1, .3)
    for dx, dy in ((-11, -4), (-4, -11), (4, -11), (11, -4)): g += sk_ell(x + dx * s, y + dy * s, 3.6 * s, 4.4 * s, fill, fill, 1, .2)
    return g
def note(x, y, s=1.0, col=DIM2):
    return '<g fill="%s" stroke="%s" stroke-width="%.1f" stroke-linecap="round"><ellipse cx="%.1f" cy="%.1f" rx="%.1f" ry="%.1f" transform="rotate(-20 %.1f %.1f)"/><path d="M%.1f %.1f L%.1f %.1f L%.1f %.1f" fill="none"/></g>' % (col, col, 2.5 * s, x, y, 7 * s, 5 * s, x, y, x + 6 * s, y, x + 6 * s, y - 24 * s, x + 16 * s, y - 20 * s)
def crate_art(x, y, w, h):
    """a plastic record crate, drawn by hand: ribs, lattice, handle slot and a row of sleeves on top"""
    g = ""
    for i in range(9):   # sleeves sticking out
        sx = x + 10 + i * (w - 30) / 9
        g += sk_poly([(sx, y), (sx + 14, y - 6), (sx + 14, y + 14), (sx, y + 20)], INK if i % 3 else GOLD, INK, 2, .6)
    g += sk_rect(x, y + 18, w, h - 18, 8, BLK, INK, 4, 1.6)
    g += sk_line(x + 6, y + 48, x + w - 6, y + 48, INK, 3, 1, 2) + sk_line(x + 6, y + h - 40, x + w - 6, y + h - 40, INK, 3, 1, 2)
    gx0, gx1, gy0, gy1 = x + 14, x + w - 14, y + 56, y + h - 48
    step = 34
    k = -int((gy1 - gy0) / step) - 1
    while gx0 + k * step < gx1 + (gy1 - gy0):
        xa = gx0 + k * step
        for sgn in (1, -1):
            xs, ys, xe, ye = (xa, gy0, xa + sgn * (gy1 - gy0), gy1)
            # clip the diagonal to the lattice box
            def clip(xs, ys, xe, ye):
                t0, t1 = 0.0, 1.0
                for p, q in ((-(xe - xs), xs - gx0), ((xe - xs), gx1 - xs), (-(ye - ys), ys - gy0), ((ye - ys), gy1 - ys)):
                    if p == 0:
                        if q < 0: return None
                    else:
                        t = q / p
                        if p < 0: t0 = max(t0, t)
                        else: t1 = min(t1, t)
                if t0 >= t1: return None
                return xs + (xe - xs) * t0, ys + (ye - ys) * t0, xs + (xe - xs) * t1, ys + (ye - ys) * t1
            c = clip(xs, ys, xe, ye)
            if c: g += sk_line(*c, stroke=INK, sw=2, j=.8, bend=1)
        k += 1
    g += sk_rect(x + w / 2 - 38, y + 26, 76, 16, 7, BLK, INK, 3, .8, False)    # handle slot
    return g

# ------------------------------------------------------------------ the platter: a record with a happy dog on the label, 48 turned pictures for the live meter
NPLAT, PS = 24, 250
def platter_base(S=500):
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0)); d = ImageDraw.Draw(im); c = S / 2
    def circ(r, fill=None, outline=None, w=1, cx=None, cy=None):
        cx = c if cx is None else cx; cy = c if cy is None else cy
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fill, outline=outline, width=w)
    circ(c - 3, fill=(14, 14, 15, 255), outline=(244, 241, 234, 255), w=6)
    for k in range(0, 9): circ(c - 20 - k * 13, outline=(52, 52, 54, 255), w=2)
    for a0 in (20, 200):                                                              # light streaks that turn with the record
        d.arc([c - 205, c - 205, c + 205, c + 205], a0, a0 + 34, fill=(120, 118, 112, 255), width=9)
        d.arc([c - 170, c - 170, c + 170, c + 170], a0 + 6, a0 + 26, fill=(86, 84, 80, 255), width=6)
    for a0 in (110, 290): d.arc([c - 215, c - 215, c + 215, c + 215], a0, a0 + 14, fill=(244, 241, 234, 255), width=5)
    circ(104, fill=(255, 181, 46, 255), outline=(244, 241, 234, 255), w=5)
    # the dog on the label (ears, head, snout, eyes, tongue)
    for sx in (-1, 1):
        d.ellipse([c + sx * 56 - 16, c - 36, c + sx * 56 + 16, c + 30], fill=(244, 241, 234, 255), outline=(14, 14, 15, 255), width=4)
    circ(58, fill=(14, 14, 15, 255), outline=(244, 241, 234, 255), w=4, cy=c + 4)
    d.ellipse([c - 30, c + 12, c + 30, c + 52], fill=(244, 241, 234, 255), outline=(14, 14, 15, 255), width=3)
    d.ellipse([c - 11, c + 14, c + 11, c + 30], fill=(14, 14, 15, 255))
    for sx in (-1, 1):
        d.ellipse([c + sx * 24 - 10, c - 30, c + sx * 24 + 10, c - 8], fill=(244, 241, 234, 255)); d.ellipse([c + sx * 24 - 5, c - 24, c + sx * 24 + 5, c - 12], fill=(14, 14, 15, 255))
    d.ellipse([c - 9, c + 36, c + 9, c + 62], fill=(255, 107, 138, 255), outline=(14, 14, 15, 255), width=2)
    circ(7, fill=(14, 14, 15, 255), outline=(244, 241, 234, 255), w=2)
    d.polygon([(c - 10, 8), (c + 10, 8), (c, 34)], fill=(255, 181, 46, 255))              # a gold marker at the rim
    return im
_base = platter_base()
_strip = Image.new("RGBA", (PS, PS * NPLAT), (0, 0, 0, 0))
for _k in range(NPLAT):
    _f = _base.rotate(-360.0 * _k / NPLAT, resample=Image.BICUBIC).resize((PS, PS), Image.LANCZOS)
    _strip.paste(_f, (0, _k * PS))
_strip.save(IMG + "/platter.png")

# ------------------------------------------------------------------ keys, rows, pads, segments
KIND = {   # fill, outline, text, shadow
    "rubber":    (BLK, INK, INK),
    "rubber_on": (INK, INK, BLK),
    "silver":    (INK, INK, BLK),
    "silver_on": (GOLD, INK, BLK),
}
def key_img(name, w, h, label, size, kind="rubber", on=False):
    fill, line, tc = KIND[kind + ("_on" if on else "")]
    body = sk_rect(6, 8, w - 8, h - 10, 11, "#000", "#000", 1, 0.3, False).replace('fill="#000"', 'fill="#000" opacity=".0"')
    body = '<path d="%s" fill="%s" opacity=".9"/>' % (smooth(rr_pts(8, 9, w - 10, h - 11, 11)), "#2c2b28")   # a hard comic shadow
    body += sk_rect(3, 3, w - 9, h - 11, 11, fill, line, 3)
    if label: body += txc(label, (w - 6) / 2, (h - 8) / 2 + 3, size, tc, "middle", "BebasNeue", 1.5)
    svg(name, w, h, body)
def pair(key, w, h, label, size, kind="rubber", fn=None):
    key_img("b_" + key, w, h, label, size, kind, False); key_img("b_%s_on" % key, w, h, label, size, kind, True)

svg("white", 304, 304, '<rect width="304" height="304" rx="8" fill="#fff"/>')
for on in (False, True):
    fill, stroke = (("#3a2606", GOLD) if on else (BLK, INK))
    for sfx, w in (("", 1200), ("_n", 720), ("_p", 700), ("_m", 240), ("_c", 600)):
        svg("row" + sfx + ("_on" if on else ""), w, 48, sk_rect(3, 3, w - 7, 40, 10, fill, stroke, 2.8))
for nm, on in (("pad", False), ("pad_on", True)):
    sh = '<path d="%s" fill="#2c2b28"/>' % smooth(rr_pts(8, 9, 176, 72, 14))
    svg(nm, 186, 84, sh + sk_rect(3, 3, 176, 72, 14, GOLD if on else BLK, INK, 3.5) + ('' if on else sk_line(16, 14, 60, 14, "#55524b", 3, .4, .6)))
for nm, on in (("seg_off", False), ("seg_on", True)):
    svg(nm, 180, 44, sk_rect(3, 3, 172, 36, 9, GOLD if on else BLK, INK, 2.8))
open(OUT + "/skin.css", "w").write(":root { --seg-size: 24px; --sheen: 0; --shadow: none; }\n.seg-tx { font-weight: 700; letter-spacing: 0.05em; }\n")
svg("frame_c", 600, 60, sk_rect(3, 3, 593, 52, 12, "#000", GOLD, 3))
svg("frame_q", 700, 60, sk_rect(3, 3, 693, 52, 12, "#000", GOLD, 3))
svg("frame_e", 700, 60, sk_rect(3, 3, 693, 52, 12, "#000", GOLD, 3))

pair("search", 300, 64, "SEARCH", 44, "silver")
pair("space", 330, 64, "SPACE", 38)
pair("kback", 170, 64, "DEL", 38)
pair("kclear", 170, 64, "CLEAR", 36)
pair("prev", 150, 56, "< PREV", 32)
pair("next", 150, 56, "NEXT >", 32)
pair("play", 170, 56, "PLAY / STOP", 30, "silver")
pair("keep", 220, 56, "KEEP", 38, "silver")
pair("topad", 380, 56, "", 1, "silver")
pair("topad2", 200, 56, "", 1, "silver")
pair("cue", 100, 56, "CUE", 34)
pair("startstop", 160, 56, "START / STOP", 28, "silver")
pair("del", 220, 56, "REMOVE", 34)
pair("padclear", 190, 52, "CLEAR PAD", 30)
pair("learnpad", 190, 52, "LEARN PAD", 30)
pair("learnbase", 390, 52, "LEARN ALL PADS", 32)
pair("test", 300, 64, "TEST", 42, "silver")
pair("register", 300, 56, "REGISTER / CHECK", 30, "silver")
pair("forget", 240, 56, "FORGET KEY", 30)
pair("minus", 56, 48, "-", 40)
pair("plus", 56, 48, "+", 40)
for i, ch in enumerate(KEYS): pair("kc%d" % i, 96, 64, ch.upper(), 42); pair("sk%d" % i, 58, 56, ch.upper(), 36)
pair("sback", 110, 56, "DEL", 32)
pair("sclear", 120, 56, "CLEAR", 30)
pair("sspace", 190, 56, "SPACE", 32)
pair("ssearch", 200, 56, "SEARCH", 40, "silver")
pair("skeep", 150, 56, "KEEP", 36, "silver")
pair("sprev", 130, 56, "< PREV", 30)
pair("snext", 130, 56, "NEXT >", 30)
pair("splay", 170, 56, "PLAY / STOP", 28, "silver")
pair("sq", 146, 52, "", 1)

for i, ch in enumerate(EKEYS): pair("ec%d" % i, 96, 56, ch.upper() if ch.isalpha() else ch, 38)
for i, w in enumerate(QUICK): pair("pq%d" % (i + 1), 136, 56, w.upper(), 30); pair("spq%d" % (i + 1), 146, 52, w.upper(), 30)

# ------------------------------------------------------------------ background (every tab): black, sketchy header, the two buddies
bg = '<rect width="1280" height="628" fill="%s"/>' % BLK
bg += sk_line(10, 94, 1270, 94, INK, 4, 1.2, 2.5) + sk_line(14, 99, 1266, 99, INK, 1.8, 1, 2)
bg += tx("CRATE DIGGA", 96, 62, 70, INK, "start", "BebasNeue", 3)
bg += sk_line(98, 70, 360, 70, GOLD, 5, 1.5, 2)
bg += tx("DIG DEEP  -  SPIN LOOSE", 98, 88, 17, GOLD, "start", "BebasNeue", 3)
bg += dog(470, 54, 0.62) + cat(570, 56, 0.62)
bg += paw(640, 30, 0.8) + note(655, 76, 0.6) + paw(30, 82, 0.5)
bg += paw(1262, 300, .8) + paw(1250, 470, .6) + note(1262, 560, .8) + note(14, 330, .8) + paw(16, 520, .7)
bg += '<rect x="696" y="16" width="560" height="64" rx="9" fill="none" stroke="%s" stroke-width="3" stroke-linecap="round" stroke-dasharray="0.1 8" opacity=".75"/>' % INK
svg("bg", 1280, 628, bg)

# ---- sample editor pictures: one column of the waveform (72 pictures: outside / inside / playing x 24 heights) and one of the envelope (24 heights)
WW, WH, EW, EH = 11, 100, 13, 100
_w = Image.new("RGBA", (WW, WH * 72), (0, 0, 0, 0)); _wd = ImageDraw.Draw(_w)
for _st, _col in enumerate(((88, 85, 78, 255), (244, 241, 234, 255), (255, 181, 46, 255))):
    for _lv in range(24):
        _y0 = (_st * 24 + _lv) * WH; _h = int(_lv / 23.0 * 46) + 1
        if _st == 2: _wd.rectangle([WW // 2 - 1, _y0 + 2, WW // 2, _y0 + WH - 3], fill=(255, 181, 46, 150))
        _wd.rectangle([1, _y0 + WH // 2 - _h, WW - 2, _y0 + WH // 2 + _h - 1], fill=_col)
_w.save(IMG + "/wave.png")
_e = Image.new("RGBA", (EW, EH * 24), (0, 0, 0, 0)); _ed = ImageDraw.Draw(_e)
for _lv in range(24):
    _h = int(_lv / 23.0 * 94) + 2
    _ed.rectangle([0, _lv * EH + EH - 2 - _h, EW - 1, _lv * EH + EH - 2], fill=(255, 181, 46, 255 if _lv else 120))
_e.save(IMG + "/env.png")
Image.new("RGBA", (12, 250), (0, 0, 0, 0)).save(IMG + "/clr_thumb.png")
Image.new("RGBA", (300, 250), (0, 0, 0, 0)).save(IMG + "/clr_track.png")
_t = Image.new("RGBA", (22, 38), (0, 0, 0, 0)); _td = ImageDraw.Draw(_t)
_td.polygon([(2, 2), (20, 2), (20, 26), (11, 36), (2, 26)], fill=(255, 181, 46, 255), outline=(244, 241, 234, 255))
_t.save(IMG + "/ed_thumb.png")
_k = Image.new("RGBA", (912, 38), (0, 0, 0, 0)); ImageDraw.Draw(_k).rounded_rectangle([0, 15, 911, 22], radius=3, fill=(88, 85, 78, 255), outline=(244, 241, 234, 255))
_k.save(IMG + "/ed_track.png")
_t2 = Image.new("RGBA", (16, 24), (0, 0, 0, 0)); ImageDraw.Draw(_t2).rounded_rectangle([1, 1, 14, 22], radius=5, fill=(255, 181, 46, 255), outline=(244, 241, 234, 255))
_t2.save(IMG + "/ed_thumb2.png")
_k2 = Image.new("RGBA", (270, 24), (0, 0, 0, 0)); ImageDraw.Draw(_k2).rounded_rectangle([0, 9, 269, 14], radius=3, fill=(88, 85, 78, 255), outline=(244, 241, 234, 255))
_k2.save(IMG + "/ed_track2.png")
for _n in range(1, 17): pair("sp%d" % _n, 70, 44, str(_n), 30)
pair("stopall", 300, 56, "STOP ALL", 38, "silver")
pair("edplay", 300, 56, "PLAY PAD", 38, "silver")

# ------------------------------------------------------------------ per-tab lettering (svg y = layout y - 86)
def dots(cx, cy, w, h):
    """a dotted white outline that shows where a live text line sits"""
    x, y = cx - w / 2, cy - 86 - h / 2
    return '<rect x="%.1f" y="%.1f" width="%d" height="%d" rx="9" fill="none" stroke="%s" stroke-width="3" stroke-linecap="round" stroke-dasharray="0.1 8" opacity=".75"/>' % (x, y, w, h, INK)
DOTS = {"lbl_search": [(330, 672, 600, 28), (880, 612, 440, 32), (1185, 612, 150, 32)],
        "lbl_crate": [(500, 540, 100, 26), (790, 584, 500, 32), (1145, 584, 190, 32), (890, 622, 700, 28)],
        "lbl_pads": [(1050, 226, 420, 32), (330, 626, 600, 30), (330, 662, 600, 28), (1050, 662, 420, 28), (1135, 282, 130, 40)],
        "lbl_edit": [(490, 268, 900, 28), (1110, 330, 300, 28), (840, 498, 270, 28), (1130, 498, 270, 28), (840, 582, 270, 28), (1130, 582, 270, 28), (330, 690, 600, 26)], "lbl_info": [(1000, 640, 440, 36), (750, 684, 1000, 28)]}
def overlay(name, parts): svg(name, 1280, 628, "".join(parts) + "".join(dots(*d) for d in DOTS.get(name, [])))
def L(s, x, y, size=18, fill="#a8a49a", anchor="start", sp=2, fn="BebasNeue"): return txc(s, x, y - 86, size, fill, anchor, fn, sp)
overlay("lbl_search", [L("SHORT ONLY", 1160, 266, 14, INK, "middle", 2)])
_arm = (sk_ell(455, 150, 22, 22, "#2a2927", INK, 3, .8) + sk_ell(455, 150, 8, 8, INK, INK, 1, .3) +
        '<path d="M455 150 L300 292" stroke="%s" stroke-width="12" stroke-linecap="round"/><path d="M455 150 L300 292" stroke="%s" stroke-width="5" stroke-linecap="round"/>' % (INK, BLK) +
        sk_rect(280, 282, 38, 20, 6, GOLD, INK, 3, .8, False).replace('/>', ' transform="rotate(-45 299 292)"/>', 1) +
        sk_rect(446, 100, 18, 40, 5, INK, INK, 2, .6, False))
overlay("lbl_crate", [sk_rect(16, 108, 504, 508, 22, "none", INK, 4, 1.8), '<circle cx="175" cy="259" r="138" fill="#050505" stroke="%s" stroke-width="3"/>' % INK, _arm,
    L("PITCH", 492, 318, 18, INK, "middle", 2), L("TOUCH THE RECORD TO SCRATCH", 175, 498, 17, INK, "middle", 2), L("RANGE", 352, 670, 15, "#a8a49a", sp=2), paw(430, 560 - 86 + 8, .6)])
overlay("lbl_pads", [L("SELECTED PAD", 852, 202, 17, "#a8a49a", sp=3)] + [L(n, 852, y, 24, INK, sp=2) for n, y in (("GROUP", 282), ("MODE", 336))] +
        [L("SAME GROUP NUMBER = PADS CUT EACH OTHER OFF", 36, 600, 17, "#a8a49a", sp=2), L("START, END, ATTACK, DECAY, SUSTAIN, RELEASE: TAB EDIT", 852, 470, 17, GOLD, sp=1), dog(780, 620 - 86 + 40, .5, False)])
overlay("lbl_edit", [sk_rect(36, 322 - 86, 900, 104, 10, "#0a0a0b", "#3a3834", 2.5), sk_rect(36, 490 - 86, 636, 104, 10, "#0a0a0b", "#3a3834", 2.5),
                     L("START", 44, 297, 15, GOLD, sp=2), L("END", 44, 461, 15, GOLD, sp=2), L("ENVELOPE", 40, 483, 16, "#a8a49a", sp=3),
                     L("MODE", 40, 622, 16, "#a8a49a", sp=3), L("MUTE GROUP", 330, 622, 16, "#a8a49a", sp=3)])
info = [("CRATE DIGGA  V" + VERSION_TXT, 40, GOLD), ("BY JJ-LEMMING  -  JJ DA PLUG", 24, INK), ("", 0, 0),
        ("WHAT IT DOES", 26, GOLD),
        ("FIND FREE MUSIC BY GENRE, WORDS OR SOUNDS: MUSIC AND SPOKEN WORD ARE KEPT APART.", 20, "#cfcbc0"),
        ("HEAR THEM, KEEP THEM, PUT THEM ON THE 16 PADS OR SPIN THEM ON THE TURNTABLE.", 20, "#cfcbc0"), ("NO PHONE, NO COMPUTER, NO ACCOUNT NEEDED.", 20, "#cfcbc0"), ("", 0, 0),
        ("LICENSE", 26, GOLD),
        ("ONLY CC0 AND PUBLIC DOMAIN SOUNDS ARE SHOWN (ARCHIVE.ORG, WIKIMEDIA, OPENVERSE).", 20, "#cfcbc0"),
        ("EVERY KEPT SOUND GETS A TEXT FILE: TITLE, CREATOR, LICENSE, SOURCE.", 20, "#cfcbc0"), ("", 0, 0),
        ("PADS AND DECK", 26, GOLD),
        ("PAD 1-16 = MIDI NOTES 36-51 (AND 0-15). STOP ENDS EVERYTHING AT ONCE.", 20, "#cfcbc0"),
        ("EDIT TAB: WAVEFORM, START AND END, ATTACK DECAY SUSTAIN RELEASE. CRATE TAB: TOUCH THE RECORD.", 20, "#cfcbc0"), ("", 0, 0),
        ("NO ACCOUNT NEEDED. EMPTY SEARCH OR NO MATCH = FREE PICKS. ONLY THE START OF LONG FILES IS LOADED.", 20, "#cfcbc0")]
parts = []; y = 214
for s_, sz, col in info:
    if s_: parts.append(L(s_, 250, y, sz, col, sp=1.5))
    y += 25 if s_ else 8
parts.append(crate_art(980, 330, 250, 150)); parts.append(dog(1040, 312, .75, True)); parts.append(cat(1160, 314, .75, True))
overlay("lbl_info", parts)

# ------------------------------------------------------------------ layout
Lc = []
def add(s): Lc.append(s)
add("# CRATE DIGGA %s - JJ DA PLUG. Five native MPC tabs. Shadow coordinates (y = skin y + 86), plugin area 1280 x 628." % VERSION_TXT)
add("art_css=skin.css\ntheme_bg=0e0e0f\ntheme_ink=f4f1ea\ntheme_accent=ffb52e\ntheme_line=f4f1ea\ntheme_seg_active_tx=0e0e0f")

def header(lbl=None):
    add("art file=images/bg.svg")
    if lbl: add("art file=images/%s.svg" % lbl)
    add("art file=images/logo.png x=24 y=90 w=52 h=82")
    add("readout box=0 w=560 h=40 cx=976 cy=124 label=\"\" key=hdr_txt tsize=34 tweight=bold tcolor=ffb52e talign=right tpad=0")
    add("readout box=0 w=560 h=30 cx=976 cy=158 label=\"\" key=status_txt tsize=22 tweight=semibold tcolor=f4f1ea talign=right tpad=0")

def rowbtn(key, get, cx, cy, w, h, img="row", size=30):
    add("button cx=%d cy=%d label=\"\" key=%s get=%s tsize=%d tweight=semibold tcolor=ffb52e talign=left img=images/%s.svg img_on=images/%s_on.svg w=%d h=%d" % (cx, cy, key, get, size, img, img, w, h))
def btn(key, cx, cy, w, h, img=None):
    img = img or key
    add("button cx=%d cy=%d label=\"\" key=%s img=images/b_%s.svg img_on=images/b_%s_on.svg w=%d h=%d" % (cx, cy, key, img, img, w, h))
def ro_(key, cx, cy, w, h, size, color, align, weight="regular"):
    add("readout box=0 w=%d h=%d cx=%d cy=%d label=\"\" key=%s tsize=%d tweight=%s tcolor=%s talign=%s tpad=0" % (w, h, cx, cy, key, size, weight, color, align))
def tab(name, lbl):
    add("\n[tab %s]" % name)
    header(lbl)
    return lambda: add('qlinks "%s" = -,-,-,-' % name)
TOPAD = "button cx=860 cy=672 label=\"\" key=to_pad get=to_pad_txt tsize=28 tweight=bold tcolor=1a1917 talign=center img=images/b_topad.svg img_on=images/b_topad_on.svg w=380 h=56"

# ---- SEARCH: keyboard on the left, the results right next to it (one tab)
q = tab("SEARCH", "lbl_search")
add("art file=images/frame_c.svg x=30 y=196 w=600 h=60")
ro_("q_txt", 330, 226, 560, 50, 38, "ffb52e", "left", "bold")
rows = ["qwertyuiop", "asdfghjkl", "zxcvbnm"]
for r, line in enumerate(rows):
    x0 = 56 + (0 if r == 0 else 30 if r == 1 else 61)
    for c, ch in enumerate(line):
        btn("kc%d" % KEYS.index(ch), x0 + c * 61, 306 + r * 62, 58, 56, "sk%d" % KEYS.index(ch))
btn("k_back", 565, 306 + 2 * 62, 110, 56, "sback")
btn("k_space", 125, 306 + 3 * 62, 190, 56, "sspace")
btn("k_clear", 285, 306 + 3 * 62, 120, 56, "sclear")
btn("do_search", 500, 306 + 3 * 62, 200, 56, "ssearch")
for i in range(4): btn("pq%d" % (i + 1), 76 + i * 152, 554, 146, 52, "spq%d" % (i + 1))
btn("keep", 110, 616, 150, 56, "skeep")
add("button cx=380 cy=616 label=\"\" key=to_pad get=to_pad_txt tsize=22 tweight=bold tcolor=1a1917 talign=center img=images/b_topad2.svg img_on=images/b_topad2_on.svg w=200 h=56")
ro_("note_txt", 330, 672, 600, 28, 20, "ffb52e", "left", "semibold")
add("enum_h cx=860 cy=226 label=\"\" key=cat sw=130 sh=50 img=images/seg_off.svg img_on=images/seg_on.svg")
add("enum_h cx=1160 cy=226 label=\"\" key=shortonly sw=100 sh=50 img=images/seg_off.svg img_on=images/seg_on.svg")
for i in range(ROWS): rowbtn("rt%d" % (i + 1), "r%d" % (i + 1), 960, 292 + i * 46, 600, 42, "row_c", 24)
ro_("rs_info", 880, 612, 440, 32, 20, "e9e7e0", "left", "semibold")
ro_("r_page_txt", 1185, 612, 150, 32, 20, "9a968c", "right")
btn("r_prev", 735, 664, 130, 56, "sprev")
btn("pv_play", 960, 664, 170, 56, "splay")
btn("r_next", 1185, 664, 130, 56, "snext")
q()
# ---- CRATE: the turntable (left) and the sounds you kept (right)
q = tab("CRATE", "lbl_crate")
add("meter cx=175 cy=345 w=250 h=250 key=plat strip=images/platter.png frames=%d" % NPLAT)
add("slider_v cx=492 cy=420 w=50 h=170 label=\"\" key=pitch look=fader ns=0 vs=0 bw=50")
ro_("pitch_txt", 500, 540, 100, 26, 18, "ffb52e", "center", "bold")
add("slider_h cx=175 cy=345 w=300 h=250 label=\"\" key=jog img=images/clr_thumb.png base=images/clr_track.png ns=0 vs=0 bw=300")   # invisible: the finger on the record is the scratch
btn("pv_play", 100, 604, 160, 56, "startstop")
btn("deck_cue", 235, 604, 100, 56, "cue")
add("enum_h cx=390 cy=604 label=\"\" key=klock sw=95 sh=50 img=images/seg_off.svg img_on=images/seg_on.svg")
add("enum_h cx=180 cy=664 label=\"\" key=prange sw=80 sh=44 img=images/seg_off.svg img_on=images/seg_on.svg")
for i in range(ROWS): rowbtn("ct%d" % (i + 1), "c%d" % (i + 1), 890, 222 + i * 52, 700, 48, "row_p")
ro_("pv_txt", 790, 584, 500, 32, 22, "e9e7e0", "left", "semibold")
ro_("c_page_txt", 1145, 584, 190, 32, 22, "9a968c", "right")
ro_("note_txt", 890, 622, 700, 28, 20, "ffb52e", "left", "semibold")
btn("c_prev", 615, 672, 150, 56, "prev")
btn("c_next", 775, 672, 150, 56, "next")
btn("c_del", 950, 672, 190, 56, "del")
add("button cx=1145 cy=672 label=\"\" key=to_pad get=to_pad_txt tsize=22 tweight=bold tcolor=1a1917 talign=center img=images/b_topad2.svg img_on=images/b_topad2_on.svg w=200 h=56")
q()
# ---- PADS: 16 pads (pad 1 bottom left like the hardware) + envelope, mute group, mode for the selected pad
q = tab("PADS", "lbl_pads")
for r in range(4):
    for c in range(4):
        n = (3 - r) * 4 + c + 1
        add("button cx=%d cy=%d label=\"\" key=pd%d get=pn%d tsize=22 tweight=semibold tcolor=e9e7e0 talign=center img=images/pad.svg img_on=images/pad_on.svg w=186 h=84" % (125 + c * 196, 246 + r * 96, n, n))
ro_("pad_info_txt", 1050, 226, 420, 32, 22, "e9e7e0", "left", "semibold")
btn("grp_m", 1040, 282, 56, 48, "minus")
ro_("grp_txt", 1135, 282, 130, 40, 26, "ffb52e", "center", "bold")
btn("grp_p", 1230, 282, 56, 48, "plus")
add("button cx=1135 cy=336 label=\"\" key=mode_t get=mode_txt tsize=26 tweight=bold tcolor=ffb52e talign=center img=images/row_m.svg img_on=images/row_m_on.svg w=240 h=48")
btn("stop_all", 1050, 410, 300, 56, "stopall")
btn("pad_clear", 950, 556, 190, 52, "padclear")
btn("learn_pad", 1150, 556, 190, 52, "learnpad")
btn("learn_base", 1050, 614, 390, 52, "learnbase")
ro_("learn_txt", 1050, 662, 420, 28, 20, "ffb52e", "center", "semibold")
ro_("last_txt", 330, 626, 600, 30, 22, "e9e7e0", "left", "semibold")
ro_("note_txt", 330, 662, 600, 28, 22, "ffb52e", "left", "semibold")
q()
# ---- EDIT: the sample editor of the selected pad: waveform with start/end, envelope picture with attack/decay/sustain/release
q = tab("EDIT", "lbl_edit")
for i in range(16): btn("pd%d" % (i + 1), 66 + i * 74, 226, 70, 44, "sp%d" % (i + 1))
ro_("ed_pad_txt", 490, 268, 900, 28, 22, "e9e7e0", "left", "semibold")
add("slider_h cx=486 cy=306 w=912 h=38 label=\"\" key=ed_start img=images/ed_thumb.png base=images/ed_track.png ns=0 vs=0 bw=912")
for i in range(NWV): add("meter cx=%d cy=374 w=%d h=%d key=wv%d strip=images/wave.png frames=72" % (46 + i * WW, WW, WH, i))
add("slider_h cx=486 cy=442 w=912 h=38 label=\"\" key=ed_end img=images/ed_thumb.png base=images/ed_track.png ns=0 vs=0 bw=912")
ro_("ed_pos_txt", 1110, 330, 300, 28, 20, "ffb52e", "left", "semibold")
btn("ed_play", 1110, 386, 280, 56, "edplay")
btn("stop_all", 1110, 450, 280, 56, "stopall")
for i in range(NEV): add("meter cx=%d cy=540 w=%d h=%d key=ev%d strip=images/env.png frames=24" % (47 + i * EW, EW, EH, i))
for n, (key, c, r) in enumerate((("ed_a", 0, 0), ("ed_d", 1, 0), ("ed_s", 0, 1), ("ed_r", 1, 1))):
    ro_(key + "_txt", 840 + c * 290, 498 + r * 84, 270, 28, 22, "ffb52e", "left", "bold")
    add("slider_h cx=%d cy=%d w=270 h=24 label=\"\" key=%s img=images/ed_thumb2.png base=images/ed_track2.png ns=0 vs=0 bw=270" % (840 + c * 290, 530 + r * 84, key))
add("button cx=165 cy=652 label=\"\" key=mode_t get=mode_txt tsize=26 tweight=bold tcolor=ffb52e talign=center img=images/row_m.svg img_on=images/row_m_on.svg w=240 h=48")
btn("grp_m", 400, 652, 56, 48, "minus")
ro_("grp_txt", 500, 652, 130, 40, 26, "ffb52e", "center", "bold")
btn("grp_p", 600, 652, 56, 48, "plus")
ro_("note_txt", 330, 690, 600, 26, 20, "ffb52e", "left", "semibold")
q()
# ---- INFO (last)
q = tab("INFO", "lbl_info")
add("art file=images/logo.png x=60 y=210 w=150 h=240")
btn("net_test", 560, 640, 300, 56, "test")
ro_("net_txt", 1000, 640, 440, 36, 24, "ffb52e", "left", "semibold")
ro_("dir_txt", 750, 684, 1000, 28, 20, "9a968c", "left")
q()
open(OUT + "/layout.conf", "w").write("\n".join(Lc) + "\n")

json.dump({"name": "Crate Digga", "vendor": "JJ DA PLUG", "uid": "CrDg", "version": 160, "so": "crate.so", "params": "params.json",
           "layout": "layout.conf", "art": "html", "defines": {"PARAM_TEXT_MAX": 128, "HAS_DISPLAY_REV": 1, "HAS_SONG_POS": 1},
           "build": {"root": ".", "sources": ["crate.c", "vorbis_glue.c"], "cflags": ["-I."], "libs": ["-lpthread", "-lm", "-ldl"]}},
          open(OUT + "/vst.json", "w"), indent=1)
print("params", len(P), "layout lines", len(Lc))
