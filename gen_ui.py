#!/usr/bin/env python3
"""Run by build.sh before the build. Generates params.json, layout.conf, vst.json and the skin images for CRATE DIGGA (JJ DA PLUG).
Look: Technics SL-1200 MK2 (silver faceplate, black rubber keys, amber LEDs) with a touch of ghetto blaster (VU ladder, speaker grille, screws).
All big lettering is turned into outlines from Michroma / Orbitron (SIL Open Font License, see OFL-*.txt), so it looks the same everywhere."""
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
QUICK = ["kick", "snare", "hihat", "clap", "bass", "808", "loop", "fx"]
VERSION_TXT = "1.1"

# ------------------------------------------------------------------ params (v1.0 order first, new ones appended at the end)
P = []
def opt(key, name, options, default=0): P.append({"key": key, "name": name, "options": options, "default": default})
def ro(key, name): P.append({"key": key, "name": name, "type": "readout", "display": "string"})
def trig(key, name): P.append({"key": key, "name": name, "type": "trigger", "momentary": True})
opt("cat", "Type", ["all", "sounds", "music"])
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
json.dump({"name": "Crate Digga", "params": P}, open(OUT + "/params.json", "w"), indent=1)

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
def text_w(s, size, fn="Michroma", spacing=0.0):
    t, gs, cm, upm, hm = font(fn)
    return sum(hm[cm.get(ord(c), cm[ord("?")])][0] * size / upm + spacing for c in s) - spacing
def tx(s, x, y, size, fill, anchor="start", fn="Michroma", spacing=0.0):
    """text as outlines; (x, y) = baseline start/middle/end"""
    t, gs, cm, upm, hm = font(fn)
    w = text_w(s, size, fn, spacing)
    cx = x - (w / 2 if anchor == "middle" else w if anchor == "end" else 0)
    out = []
    sc = size / upm
    for c in s:
        g = cm.get(ord(c), cm[ord("?")])
        d = glyph_d(fn, g)
        if d: out.append('<path transform="translate(%.2f %.2f) scale(%.5f %.5f)" d="%s"/>' % (cx, y, sc, -sc, d))
        cx += hm[g][0] * sc + spacing
    return '<g fill="%s">%s</g>' % (fill, "".join(out))
def txc(s, x, y, size, fill, anchor="middle", fn="Michroma", spacing=0.0):
    """vertically centred on y (cap height approx 0.7 em)"""
    return tx(s, x, y + size * 0.35, size, fill, anchor, fn, spacing)

def svg(name, w, h, body, defs=""):
    open("%s/%s.svg" % (IMG, name), "w").write('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d"><defs>%s</defs>%s</svg>' % (w, h, w, h, defs, body))
def lin(gid, stops, vertical=True):
    s = '<linearGradient id="%s" x1="0" y1="0" x2="%d" y2="%d">' % (gid, 0 if vertical else 1, 1 if vertical else 0)
    for off, col in stops: s += '<stop offset="%s" stop-color="%s"/>' % (off, col)
    return s + "</linearGradient>"

# ------------------------------------------------------------------ keys (black rubber / silver / lit amber)
KIND = {  # top, bottom, rim, text, rim width
    "rubber":    ("#3b3a37", "#161513", "#8e8b83", SILVER),
    "rubber_on": ("#5a3d08", "#2d1d04", AMBER, "#ffd27a"),
    "silver":    ("#f4f3ee", "#a9a69d", "#ffffff", "#1a1917"),
    "silver_on": ("#ffd27a", "#d58f12", "#fff2cf", "#1a1917"),
}
def key_img(name, w, h, label, size, kind="rubber", on=False, fn="Michroma"):
    top, bot, rim, tc = KIND[kind + ("_on" if on else "")]
    body = '<rect x="2" y="2" width="%d" height="%d" rx="7" fill="url(#k)" stroke="%s" stroke-width="2"/>' % (w - 4, h - 4, rim)
    body += '<rect x="5" y="4" width="%d" height="%d" rx="5" fill="none" stroke="#fff" stroke-opacity="%s" stroke-width="1"/>' % (w - 10, h // 2 - 3, "0.35" if kind.startswith("silver") else "0.10")
    if label: body += txc(label, w / 2, h / 2 + 1, size, tc, "middle", fn, 1)
    svg(name, w, h, body, lin("k", [(0, top), (1, bot)]))
def pair(key, w, h, label, size, kind="rubber", fn="Michroma"):
    key_img("b_" + key, w, h, label, size, kind, False, fn)
    key_img("b_%s_on" % key, w, h, label, size, kind, True, fn)

# ------------------------------------------------------------------ background: black body, silver faceplate, VU ladder, screws, grille
def screw(x, y):
    return ('<circle cx="%d" cy="%d" r="7" fill="url(#sc)" stroke="#2a2927" stroke-width="1"/>'
            '<path d="M%d %dL%d %d" stroke="#2a2927" stroke-width="2" transform="rotate(35 %d %d)"/>' % (x, y, x - 4, y, x + 4, y, x, y))
def led_ladder(x, y, n=12, lit=7):
    s = ""
    for i in range(n):
        col = "#58d26b" if i < 7 else AMBER if i < 10 else "#ff4a3a"
        on = i < lit
        s += '<rect x="%d" y="%d" width="9" height="14" rx="2" fill="%s" opacity="%s"/>' % (x + i * 12, y, col, "1" if on else "0.22")
    return s
defs = lin("body", [(0, "#232220"), (1, "#0d0c0b")]) + lin("face", [(0, "#f2f1ec"), (0.5, "#cfccc3"), (1, "#a8a59b")])
defs += '<radialGradient id="sc"><stop offset="0" stop-color="#d8d6ce"/><stop offset="1" stop-color="#6e6b63"/></radialGradient>'
defs += '<filter id="brush" x="0" y="0" width="100%" height="100%"><feTurbulence type="fractalNoise" baseFrequency="0.002 0.9" numOctaves="2" seed="3"/><feColorMatrix values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 0.22 0"/></filter>'
defs += '<pattern id="grille" width="14" height="14" patternUnits="userSpaceOnUse"><circle cx="7" cy="7" r="2.3" fill="#000" opacity=".55"/><circle cx="7" cy="7" r="2.3" fill="none" stroke="#fff" stroke-opacity=".05"/></pattern>'
bg = '<rect width="1280" height="628" fill="url(#body)"/>'
bg += '<rect x="10" y="102" width="1260" height="516" rx="12" fill="#0b0a09"/><rect x="10" y="102" width="1260" height="516" rx="12" fill="url(#grille)"/>'
bg += '<rect x="10" y="102" width="1260" height="516" rx="12" fill="none" stroke="#4a4842" stroke-width="2"/>'
# faceplate
bg += '<rect x="0" y="0" width="1280" height="92" fill="url(#face)"/><rect x="0" y="0" width="1280" height="92" filter="url(#brush)"/>'
bg += '<rect y="92" width="1280" height="3" fill="#2b2a27"/><rect y="0" width="1280" height="2" fill="#fff" opacity=".7"/>'
bg += screw(1250, 14) + screw(30, 78) + screw(1250, 78)
bg += tx("CRATE DIGGA", 98, 52, 30, "#1b1a18", "start", "Michroma", 2)
bg += tx("DIGITAL SAMPLE DECK", 100, 74, 11, "#4a4740", "start", "Michroma", 3)
bg += '<rect x="440" y="24" width="86" height="34" rx="4" fill="#1b1a18"/>' + txc("MK2", 483, 41, 22, AMBER, "middle", "Orbitron", 3)
bg += txc("33", 575, 30, 14, "#1b1a18", "middle", "Orbitron", 1) + txc("45", 615, 30, 14, "#1b1a18", "middle", "Orbitron", 1)
bg += '<circle cx="575" cy="55" r="9" fill="%s"/><circle cx="615" cy="55" r="9" fill="#5a5750"/>' % AMBER
bg += led_ladder(440, 70, 12, 8).replace('height="14"', 'height="10"').replace('y="70"', 'y="70"')
svg("bg", 1280, 628, bg, defs)
svg("white", 304, 304, '<rect width="304" height="304" rx="8" fill="#fff"/>')
svg("frame", 496, 304, '<rect x="1" y="1" width="494" height="302" rx="8" fill="#060505" stroke="%s" stroke-width="2"/>' % GREY)
for on in (False, True):
    fill, stroke = (("#3a2606", AMBER) if on else ("#0e0d0b", "#4f4c45"))
    for sfx, w in (("", 1200), ("_n", 720), ("_p", 700), ("_m", 240)):
        svg("row" + sfx + ("_on" if on else ""), w, 48, '<rect x="1" y="1" width="%d" height="46" rx="6" fill="%s" stroke="%s" stroke-width="2"/>' % (w - 2, fill, stroke))
for nm, on in (("pad", False), ("pad_on", True)):
    top, bot, stroke = (("#ffd27a", "#d58f12", "#fff2cf") if on else ("#34332f", "#121110", "#8e8b83"))
    svg(nm, 186, 84, '<rect x="2" y="2" width="182" height="80" rx="9" fill="url(#g)" stroke="%s" stroke-width="2.5"/><rect x="8" y="8" width="170" height="68" rx="6" fill="none" stroke="#000" stroke-opacity=".35"/>' % stroke, lin("g", [(0, top), (1, bot)]))
for nm, on in (("seg_off", False), ("seg_on", True)):
    fill, stroke = ((AMBER2, "#ffe0a0") if on else ("#1a1917", "#6b675e"))
    svg(nm, 180, 44, '<rect x="1" y="1" width="178" height="42" rx="6" fill="%s" stroke="%s" stroke-width="2"/>' % (fill, stroke))
open(OUT + "/skin.css", "w").write(":root { --seg-size: 24px; --sheen: 0; --shadow: none; }\n.seg-tx { font-weight: 700; letter-spacing: 0.05em; }\n")
svg("frame_q", 700, 60, '<rect x="1" y="1" width="698" height="58" rx="6" fill="#060505" stroke="%s" stroke-width="2"/><rect x="5" y="5" width="690" height="50" rx="4" fill="none" stroke="#3a2606"/>' % AMBER2)
svg("frame_e", 700, 60, '<rect x="1" y="1" width="698" height="58" rx="6" fill="#060505" stroke="%s" stroke-width="2"/><rect x="5" y="5" width="690" height="50" rx="4" fill="none" stroke="#3a2606"/>' % AMBER2)

pair("search", 300, 64, "SEARCH", 22, "silver")
pair("space", 330, 64, "SPACE", 18)
pair("kback", 170, 64, "DEL", 18)
pair("kclear", 170, 64, "CLEAR", 17)
pair("prev", 150, 56, "< PREV", 15)
pair("next", 150, 56, "NEXT >", 15)
pair("play", 170, 56, "PLAY/STOP", 14, "silver")
pair("keep", 220, 56, "KEEP", 20, "silver")
pair("topad", 380, 56, "", 1, "silver")
pair("del", 220, 56, "REMOVE", 17)
pair("padclear", 190, 52, "CLEAR PAD", 14)
pair("learnpad", 190, 52, "LEARN PAD", 14)
pair("learnbase", 390, 52, "LEARN ALL PADS", 15)
pair("test", 300, 64, "TEST", 24, "silver")
pair("register", 300, 56, "REGISTER", 19, "silver")
pair("forget", 240, 56, "FORGET KEY", 14)
pair("minus", 56, 48, "-", 22)
pair("plus", 56, 48, "+", 22)
for i, ch in enumerate(KEYS): pair("kc%d" % i, 96, 64, ch.upper(), 22)
for i, ch in enumerate(EKEYS): pair("ec%d" % i, 96, 56, ch.upper() if ch.isalpha() else ch, 20)
for i, w in enumerate(QUICK): pair("pq%d" % (i + 1), 136, 56, w.upper(), 14)

# ------------------------------------------------------------------ per-tab lettering (outlines, svg y = layout y - 86)
def overlay(name, parts): svg(name, 1280, 628, "".join(parts))
def L(s, x, y, size=13, fill=GREY, anchor="start", sp=1.5, fn="Michroma"): return txc(s, x, y - 86, size, fill, anchor, fn, sp)
overlay("lbl_search", [L("SHORT ONLY", 1130, 366, 11, GREY, "middle", 2)])
overlay("lbl_results", [])
overlay("lbl_crate", [])
overlay("lbl_pads", [L("SELECTED PAD", 852, 202, 10, DIM, sp=3)] + [L(n, 852, y, 13, SILVER, sp=2) for n, y in
                    (("ATTACK", 282), ("DECAY", 336), ("RELEASE", 390), ("GROUP", 444), ("MODE", 498))] +
        [L("SAME GROUP NUMBER = PADS CUT EACH OTHER OFF", 36, 600, 10, DIM, sp=1.5)])
overlay("lbl_account", [L("1  TYPE YOUR E-MAIL", 770, 206, 10, GREY, sp=2), L("2  PRESS REGISTER", 770, 224, 10, GREY, sp=2),
                        L("3  OPEN THE MAIL, TAP THE LINK", 770, 242, 10, GREY, sp=2), L("4  PRESS TEST ON THE INFO TAB", 770, 260, 10, GREY, sp=2)])
info = [("CRATE DIGGA  V" + VERSION_TXT, 20, AMBER), ("BY JJ-LEMMING  -  JJ DA PLUG", 12, SILVER), ("", 0, 0),
        ("WHAT IT DOES", 12, AMBER),
        ("SEARCH FREE SOUNDS ONLINE, HEAR THEM, KEEP THEM IN YOUR CRATE,", 11, GREY),
        ("PUT THEM ON THE 16 PADS. NO PHONE, NO COMPUTER NEEDED.", 11, GREY), ("", 0, 0),
        ("LICENSE", 12, AMBER),
        ("ONLY CC0 AND PUBLIC DOMAIN SOUNDS ARE SHOWN (SOURCE: OPENVERSE.ORG).", 11, GREY),
        ("EVERY KEPT SOUND GETS A TEXT FILE: TITLE, CREATOR, LICENSE, SOURCE.", 11, GREY), ("", 0, 0),
        ("PADS", 12, AMBER),
        ("READY: PAD 1-16 = MIDI NOTES 36-51 (AND 0-15). NOTHING TO LEARN.", 11, GREY),
        ("ATTACK, DECAY, RELEASE AND MUTE GROUP PER PAD. STOP SILENCES ALL.", 11, GREY), ("", 0, 0),
        ("SEARCH SAYS 'LIMIT'?  ->  ACCOUNT TAB: FREE KEY, ONE TIME.", 11, GREY)]
parts = []; y = 214
for s, sz, col in info:
    if s: parts.append(L(s, 250, y, sz, col, sp=1.5))
    y += 26 if s else 12
overlay("lbl_info", parts)

# ------------------------------------------------------------------ layout
Lc = []
def add(s): Lc.append(s)
add("# CRATE DIGGA %s - JJ DA PLUG. Six native MPC tabs. Shadow coordinates (y = skin y + 86), plugin area 1280 x 628." % VERSION_TXT)
add("art_css=skin.css\ntheme_bg=0d0c0b\ntheme_ink=e9e7e0\ntheme_accent=ffb52e\ntheme_line=6b675e\ntheme_seg_active_tx=1a1917")

def header(lbl=None):
    add("art file=images/bg.svg")
    if lbl: add("art file=images/%s.svg" % lbl)
    add("art file=images/logo.png x=24 y=90 w=50 h=80")
    add("readout box=0 w=560 h=40 cx=976 cy=124 label=\"\" key=hdr_txt tsize=34 tweight=bold tcolor=1a1917 talign=right tpad=0")
    add("readout box=0 w=560 h=30 cx=976 cy=158 label=\"\" key=status_txt tsize=22 tweight=semibold tcolor=3a352c talign=right tpad=0")

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

# ---- SEARCH
q = tab("SEARCH", "lbl_search")
add("art file=images/frame_q.svg x=30 y=196 w=700 h=60")
ro_("q_txt", 380, 226, 660, 50, 40, "ffb52e", "left", "bold")
add("enum_h cx=1000 cy=226 label=\"\" key=cat sw=130 sh=50 img=images/seg_off.svg img_on=images/seg_on.svg")
rows = ["qwertyuiop", "asdfghjkl", "zxcvbnm"]
y = 296
for r, line in enumerate(rows):
    x0 = 80 + (0 if r == 0 else 52 if r == 1 else 104)
    for c, ch in enumerate(line):
        btn("kc%d" % KEYS.index(ch), x0 + c * 104, y + r * 72, 96, 64)
btn("k_back", 950, 296 + 2 * 72, 170, 64, "kback")
btn("k_space", 250, 296 + 3 * 72, 330, 64, "space")
btn("k_clear", 500, 296 + 3 * 72, 170, 64, "kclear")
btn("do_search", 770, 296 + 3 * 72, 300, 64, "search")
add("enum_h cx=1130 cy=410 label=\"\" key=shortonly sw=100 sh=50 img=images/seg_off.svg img_on=images/seg_on.svg")
for i in range(8): btn("pq%d" % (i + 1), 100 + i * 150, 600, 136, 56)
ro_("note_txt", 640, 650, 1200, 28, 22, "ffb52e", "left", "semibold")
q()
# ---- RESULTS
q = tab("RESULTS", "lbl_results")
for i in range(ROWS): rowbtn("rt%d" % (i + 1), "r%d" % (i + 1), 640, 222 + i * 52, 1200, 48)
ro_("rs_info", 520, 584, 920, 32, 24, "e9e7e0", "left", "semibold")
ro_("r_page_txt", 1130, 584, 300, 32, 24, "9a968c", "right")
ro_("note_txt", 640, 620, 1200, 28, 22, "ffb52e", "left", "semibold")
btn("r_prev", 100, 672, 150, 56, "prev")
btn("pv_play", 290, 672, 170, 56, "play")
btn("keep", 490, 672, 220, 56)
add(TOPAD)
btn("r_next", 1170, 672, 150, 56, "next")
q()
# ---- CRATE
q = tab("CRATE", "lbl_crate")
for i in range(ROWS): rowbtn("ct%d" % (i + 1), "c%d" % (i + 1), 640, 222 + i * 52, 1200, 48)
ro_("pv_txt", 520, 584, 920, 32, 24, "e9e7e0", "left", "semibold")
ro_("c_page_txt", 1130, 584, 300, 32, 24, "9a968c", "right")
ro_("note_txt", 640, 620, 1200, 28, 22, "ffb52e", "left", "semibold")
btn("c_prev", 100, 672, 150, 56, "prev")
btn("pv_play", 290, 672, 170, 56, "play")
btn("c_del", 490, 672, 220, 56, "del")
add(TOPAD)
btn("c_next", 1170, 672, 150, 56, "next")
q()
# ---- PADS: 16 pads (pad 1 bottom left like the hardware) + envelope, mute group, mode for the selected pad
q = tab("PADS", "lbl_pads")
for r in range(4):
    for c in range(4):
        n = (3 - r) * 4 + c + 1
        add("button cx=%d cy=%d label=\"\" key=pd%d get=pn%d tsize=22 tweight=semibold tcolor=e9e7e0 talign=center img=images/pad.svg img_on=images/pad_on.svg w=186 h=84" % (125 + c * 196, 246 + r * 96, n, n))
ro_("pad_info_txt", 1050, 226, 420, 32, 22, "e9e7e0", "left", "semibold")
for key, y in (("att", 282), ("dec", 336), ("rel", 390), ("grp", 444)):
    btn(key + "_m", 1040, y, 56, 48, "minus")
    ro_(key + "_txt", 1135, y, 130, 40, 26, "ffb52e", "center", "bold")
    btn(key + "_p", 1230, y, 56, 48, "plus")
add("button cx=1135 cy=498 label=\"\" key=mode_t get=mode_txt tsize=26 tweight=bold tcolor=ffb52e talign=center img=images/row_m.svg img_on=images/row_m_on.svg w=240 h=48")
btn("pad_clear", 950, 556, 190, 52, "padclear")
btn("learn_pad", 1150, 556, 190, 52, "learnpad")
btn("learn_base", 1050, 614, 390, 52, "learnbase")
ro_("learn_txt", 1050, 662, 420, 28, 20, "ffb52e", "center", "semibold")
ro_("last_txt", 330, 626, 600, 30, 22, "e9e7e0", "left", "semibold")
ro_("note_txt", 330, 662, 600, 28, 22, "ffb52e", "left", "semibold")
q()
# ---- ACCOUNT: a free Openverse key so searching is not limited
q = tab("ACCOUNT", "lbl_account")
add("art file=images/frame_e.svg x=30 y=196 w=700 h=60")
ro_("email_txt", 380, 226, 660, 50, 34, "ffb52e", "left", "bold")
ek = [list(range(26, 36)), list(range(0, 10)), list(range(10, 19)), list(range(19, 26))]
for r, idx in enumerate(ek):
    x0 = 80 + (0 if r < 2 else 52 if r == 2 else 104)
    for c, i in enumerate(idx): btn("ec%d" % i, x0 + c * 104, 336 + r * 62, 96, 56)
for c, i in enumerate((36, 37, 38, 39)): btn("ec%d" % i, 130 + c * 104, 336 + 4 * 62, 96, 56)
btn("e_back", 580, 336 + 4 * 62, 170, 56, "kback")
btn("e_clear", 770, 336 + 4 * 62, 170, 56, "kclear")
btn("register", 220, 652, 300, 56, "register")
btn("acc_forget", 1100, 652, 240, 56, "forget")
ro_("acc_txt", 640, 652, 500, 30, 22, "e9e7e0", "center", "semibold")
ro_("note_txt", 640, 698, 1200, 22, 20, "ffb52e", "left", "semibold")
q()
# ---- INFO (last)
q = tab("INFO", "lbl_info")
add("art file=images/logo.png x=60 y=210 w=150 h=240")
btn("net_test", 560, 640, 300, 56, "test")
ro_("net_txt", 1000, 640, 440, 36, 24, "ffb52e", "left", "semibold")
ro_("dir_txt", 750, 684, 1000, 28, 20, "9a968c", "left")
q()
open(OUT + "/layout.conf", "w").write("\n".join(Lc) + "\n")

json.dump({"name": "Crate Digga", "vendor": "JJ DA PLUG", "uid": "CrDg", "version": 110, "so": "crate.so", "params": "params.json",
           "layout": "layout.conf", "art": "html", "defines": {"PARAM_TEXT_MAX": 128, "HAS_DISPLAY_REV": 1, "HAS_SONG_POS": 1},
           "build": {"root": ".", "sources": ["crate.c", "vorbis_glue.c"], "cflags": ["-I."], "libs": ["-lpthread", "-lm", "-ldl"]}},
          open(OUT + "/vst.json", "w"), indent=1)
print("params", len(P), "layout lines", len(Lc))
