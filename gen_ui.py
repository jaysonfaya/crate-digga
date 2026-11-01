#!/usr/bin/env python3
"""Run by build.sh before the build. Generates params.json, layout.conf, vst.json and the skin images for CRATE DIGGA (JJ DA PLUG)."""
import json, os, shutil
OUT = os.path.dirname(os.path.abspath(__file__))
IMG = OUT + "/images"
os.makedirs(IMG, exist_ok=True)
if os.path.exists(OUT + "/logo.png") and os.path.abspath(OUT + "/logo.png") != os.path.abspath(IMG + "/logo.png"):
    shutil.copy(OUT + "/logo.png", IMG + "/logo.png")
ROWS = 7
G1, G2, G3 = "#f0d98c", "#c9a24a", "#8a6a22"
INK, PANEL, PANEL2, LINE = "#f2e8cc", "#17130d", "#241d10", "#6f5a2a"
KEYS = "qwertyuiopasdfghjklzxcvbnm"
QUICK = ["kick", "snare", "hihat", "clap", "bass", "808", "loop", "fx"]

# ------------------------------------------------------------------ params
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
json.dump({"name": "Crate Digga", "params": P}, open(OUT + "/params.json", "w"), indent=1)

from PIL import Image
# ------------------------------------------------------------------ images
FONT = "'Titillium Web','Segoe UI',Arial,Helvetica,sans-serif"
def svg(name, w, h, body):
    open("%s/%s.svg" % (IMG, name), "w").write('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">%s</svg>' % (w, h, w, h, body))
def gradient(gid, c1, c2, c3=None):
    s = '<linearGradient id="%s" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="%s"/>' % (gid, c1)
    if c3: s += '<stop offset=".5" stop-color="%s"/>' % c2
    s += '<stop offset="1" stop-color="%s"/></linearGradient>' % (c3 or c2)
    return s
def button_img(name, w, h, label, size, on=False, style="gold"):
    if style == "gold":      # solid gold, dark text
        top, bot, stroke, tc = ((G1, G3, "#fff3c8", "#1b1408") if not on else ("#fff6d0", "#d9b45a", "#ffffff", "#1b1408"))
    else:                    # dark with a gold line
        top, bot, stroke, tc = ((PANEL2, "#120e07", LINE, G1) if not on else ("#4a3a16", "#2b2210", G1, "#ffffff"))
    body = "<defs>%s</defs>" % gradient("g", top, bot)
    body += '<rect x="2" y="2" width="%d" height="%d" rx="10" fill="url(#g)" stroke="%s" stroke-width="2"/>' % (w - 4, h - 4, stroke)
    if label:
        body += '<text x="%d" y="%d" fill="%s" font-family="%s" font-weight="700" font-size="%d" text-anchor="middle" dominant-baseline="central" letter-spacing="1">%s</text>' % (w // 2, h // 2 + 2, tc, FONT, size, label)
    svg(name, w, h, body)
def pair(key, w, h, label, size, style="gold"):
    button_img("b_" + key, w, h, label, size, False, style)
    button_img("b_%s_on" % key, w, h, label, size, True, style)

bg = "<defs>%s%s</defs>" % (gradient("bg", "#15110a", "#0a0805"), gradient("hd", "#2a2112", "#15110a"))
bg += '<rect width="1280" height="628" fill="url(#bg)"/><rect width="1280" height="94" fill="url(#hd)"/>'
bg += '<rect y="93" width="1280" height="2" fill="%s"/><rect y="97" width="1280" height="1" fill="%s" opacity=".5"/>' % (G2, G3)
bg += '<rect x="14" y="106" width="1252" height="512" rx="12" fill="%s" stroke="%s" stroke-width="1.5"/>' % (PANEL, LINE)
svg("bg", 1280, 628, bg)
svg("white", 304, 304, '<rect width="304" height="304" rx="8" fill="#fff"/>')
svg("frame", 496, 304, '<rect x="1" y="1" width="494" height="302" rx="8" fill="#0a0805" stroke="%s" stroke-width="2"/>' % G2)
for on in (False, True):
    fill, stroke = ((("#3a2d12", G1) if on else (PANEL2, "#4a3b1a")))
    for sfx, w in (("", 1200), ("_n", 720), ("_p", 700)):
        svg("row" + sfx + ("_on" if on else ""), w, 48, '<rect x="1" y="1" width="%d" height="46" rx="9" fill="%s" stroke="%s" stroke-width="2"/>' % (w - 2, fill, stroke))
for nm, on in (("pad", False), ("pad_on", True)):
    top, bot, stroke = (("#f3dc90", "#b88f30", "#fff3c8") if on else ("#2b2312", "#18120a", LINE))
    svg(nm, 186, 84, "<defs>%s</defs>" % gradient("g", top, bot) + '<rect x="2" y="2" width="182" height="80" rx="10" fill="url(#g)" stroke="%s" stroke-width="2"/>' % stroke)
for nm, on in (("seg_off", False), ("seg_on", True)):
    fill, stroke = ((G2, "#fff3c8") if on else (PANEL2, LINE))
    svg(nm, 180, 44, '<rect x="1" y="1" width="178" height="42" rx="8" fill="%s" stroke="%s" stroke-width="2"/>' % (fill, stroke))
open(OUT + "/skin.css", "w").write(":root { --seg-size: 24px; --sheen: 0; --shadow: none; }\n.seg-tx { font-weight: 700; letter-spacing: 0.05em; }\n")
svg("frame_q", 700, 60, '<rect x="1" y="1" width="698" height="58" rx="9" fill="#0a0805" stroke="%s" stroke-width="2"/>' % G2)
pair("search", 300, 64, "SEARCH", 34)
pair("space", 330, 64, "SPACE", 28, "dark")
pair("kback", 170, 64, "&#9003;", 34, "dark")
pair("kclear", 170, 64, "CLEAR", 26, "dark")
pair("prev", 150, 56, "&#9666; PREV", 26, "dark")
pair("next", 150, 56, "NEXT &#9656;", 26, "dark")
pair("play", 170, 56, "PLAY / STOP", 22, "dark")
pair("keep", 220, 56, "KEEP", 30)
pair("topad", 380, 56, "", 1)
pair("del", 220, 56, "REMOVE", 28, "dark")
pair("padclear", 380, 60, "CLEAR PAD", 30, "dark")
pair("learnpad", 380, 60, "LEARN THIS PAD", 28, "dark")
pair("learnbase", 380, 60, "LEARN ALL PADS", 28, "dark")
pair("test", 300, 64, "TEST", 34)
for i, ch in enumerate(KEYS): pair("kc%d" % i, 96, 64, ch.upper(), 34, "dark")
for i, w in enumerate(QUICK): pair("pq%d" % (i + 1), 136, 56, w.upper(), 24, "dark")

# ------------------------------------------------------------------ layout
L = []
def add(s): L.append(s)
add("# CRATE DIGGA 1.0 - JJ DA PLUG. Five native MPC tabs. Shadow coordinates (y = skin y + 86), plugin area 1280 x 628.")
add("art_css=skin.css\ntheme_bg=0a0805\ntheme_ink=f2e8cc\ntheme_accent=c9a24a\ntheme_line=6f5a2a\ntheme_seg_active_tx=1b1408")

def header():
    add("art file=images/bg.svg")
    add("art file=images/logo.png x=24 y=92 w=50 h=80")
    add('text cx=92 cy=122 label="CRATE DIGGA" size=4 color=f2e8cc align=left weight=700 spacing=3')
    add('text cx=94 cy=158 label="JJ DA PLUG" size=2 color=c9a24a align=left weight=600 spacing=6')
    add("readout box=0 w=560 h=40 cx=976 cy=124 label=\"\" key=hdr_txt tsize=34 tweight=bold tcolor=f0d98c talign=right tpad=0")
    add("readout box=0 w=560 h=30 cx=976 cy=158 label=\"\" key=status_txt tsize=22 tweight=regular tcolor=b9a46a talign=right tpad=0")

def rowbtn(key, get, cx, cy, w, h, img="row", size=32):
    add("button cx=%d cy=%d label=\"\" key=%s get=%s tsize=%d tweight=semibold tcolor=f2e8cc talign=left img=images/%s.svg img_on=images/%s_on.svg w=%d h=%d" % (cx, cy, key, get, size, img, img, w, h))
def btn(key, cx, cy, w, h, img=None):
    img = img or key
    add("button cx=%d cy=%d label=\"\" key=%s img=images/b_%s.svg img_on=images/b_%s_on.svg w=%d h=%d" % (cx, cy, key, img, img, w, h))
def ro_(key, cx, cy, w, h, size, color, align, weight="regular"):
    add("readout box=0 w=%d h=%d cx=%d cy=%d label=\"\" key=%s tsize=%d tweight=%s tcolor=%s talign=%s tpad=0" % (w, h, cx, cy, key, size, weight, color, align))
def txt(cx, cy, label, size, color, align, weight=600, spacing=2):
    add('text cx=%d cy=%d label="%s" size=%s color=%s align=%s weight=%d spacing=%d' % (cx, cy, label, size, color, align, weight, spacing))
def tab(name):
    add("\n[tab %s]" % name)
    header()
    return lambda: add('qlinks "%s" = -,-,-,-' % name)

# ---- SEARCH: a word (on-screen keyboard) or a preset; only CC0 and public domain is ever asked for
q = tab("SEARCH")
add("art file=images/frame_q.svg x=30 y=196 w=700 h=60")
ro_("q_txt", 380, 226, 660, 50, 40, "f0d98c", "left", "bold")
add("enum_h cx=1000 cy=226 label=\"\" key=cat sw=130 sh=50 img=images/seg_off.svg img_on=images/seg_on.svg")
rows = ["qwertyuiop", "asdfghjkl", "zxcvbnm"]
y = 296
for r, line in enumerate(rows):
    x0 = 80 + (0 if r == 0 else 52 if r == 1 else 104)
    for c, ch in enumerate(line):
        i = KEYS.index(ch)
        btn("kc%d" % i, x0 + c * 104, y + r * 72, 96, 64)
btn("k_back", 950, 296 + 2 * 72, 170, 64, "kback")
btn("k_space", 250, 296 + 3 * 72, 330, 64, "space")
btn("k_clear", 500, 296 + 3 * 72, 170, 64, "kclear")
btn("do_search", 770, 296 + 3 * 72, 300, 64, "search")
txt(1130, 366, "SHORT ONLY", 1.7, "b9a46a", "center")
add("enum_h cx=1130 cy=410 label=\"\" key=shortonly sw=100 sh=50 img=images/seg_off.svg img_on=images/seg_on.svg")
for i in range(8): btn("pq%d" % (i + 1), 100 + i * 150, 600, 136, 56)
ro_("note_txt", 640, 650, 1200, 28, 22, "f0d98c", "left", "semibold")
q()
# ---- RESULTS: tap a row to hear it
q = tab("RESULTS")
for i in range(ROWS): rowbtn("rt%d" % (i + 1), "r%d" % (i + 1), 640, 222 + i * 52, 1200, 48)
ro_("rs_info", 520, 584, 920, 32, 24, "f2e8cc", "left", "semibold")
ro_("r_page_txt", 1130, 584, 300, 32, 24, "b9a46a", "right")
ro_("note_txt", 640, 620, 1200, 28, 22, "f0d98c", "left", "semibold")
btn("r_prev", 100, 672, 150, 56, "prev")
btn("pv_play", 290, 672, 170, 56, "play")
btn("keep", 490, 672, 220, 56)
add("button cx=860 cy=672 label=\"\" key=to_pad get=to_pad_txt tsize=30 tweight=bold tcolor=1b1408 talign=center img=images/b_topad.svg img_on=images/b_topad_on.svg w=380 h=56")
btn("r_next", 1170, 672, 150, 56, "next")
q()
# ---- CRATE: the sounds you kept
q = tab("CRATE")
for i in range(ROWS): rowbtn("ct%d" % (i + 1), "c%d" % (i + 1), 640, 222 + i * 52, 1200, 48)
ro_("pv_txt", 520, 584, 920, 32, 24, "f2e8cc", "left", "semibold")
ro_("c_page_txt", 1130, 584, 300, 32, 24, "b9a46a", "right")
ro_("note_txt", 640, 620, 1200, 28, 22, "f0d98c", "left", "semibold")
btn("c_prev", 100, 672, 150, 56, "prev")
btn("pv_play", 290, 672, 170, 56, "play")
btn("c_del", 490, 672, 220, 56, "del")
add("button cx=860 cy=672 label=\"\" key=to_pad get=to_pad_txt tsize=30 tweight=bold tcolor=1b1408 talign=center img=images/b_topad.svg img_on=images/b_topad_on.svg w=380 h=56")
btn("c_next", 1170, 672, 150, 56, "next")
q()
# ---- PADS: 16 pads, pad 1 bottom left like the hardware
q = tab("PADS")
for r in range(4):
    for c in range(4):
        n = (3 - r) * 4 + c + 1
        add("button cx=%d cy=%d label=\"\" key=pd%d get=pn%d tsize=24 tweight=semibold tcolor=f2e8cc talign=center img=images/pad.svg img_on=images/pad_on.svg w=186 h=84" % (125 + c * 196, 246 + r * 96, n, n))
txt(900, 226, "SELECTED PAD", 1.9, "b9a46a", "left")
ro_("pad_info_txt", 1050, 262, 420, 36, 24, "f2e8cc", "center", "semibold")
btn("pad_clear", 1050, 340, 380, 60, "padclear")
btn("learn_pad", 1050, 420, 380, 60, "learnpad")
btn("learn_base", 1050, 500, 380, 60, "learnbase")
ro_("learn_txt", 1050, 566, 420, 30, 22, "f0d98c", "center", "semibold")
txt(1050, 604, "PLAY A SOUND, PICK A PAD, PRESS TO PAD", 1.4, "8a7440", "center", 400, 1)
ro_("note_txt", 640, 650, 1200, 28, 22, "f0d98c", "left", "semibold")
q()
# ---- INFO
q = tab("INFO")
txt(40, 236, "ONLY CC0 AND PUBLIC DOMAIN SOUNDS ARE EVER SHOWN.", 2.2, "f0d98c", "left")
txt(40, 280, "SOURCE: OPENVERSE (OPENVERSE.ORG), WHICH COLLECTS FREE AUDIO FROM FREESOUND,", 1.8, "b9a46a", "left", 400, 1)
txt(40, 312, "JAMENDO, THE INTERNET ARCHIVE AND OTHERS. A KEPT SOUND COMES WITH A TEXT FILE", 1.8, "b9a46a", "left", 400, 1)
txt(40, 344, "THAT NAMES TITLE, CREATOR, LICENSE AND SOURCE ADDRESS.", 1.8, "b9a46a", "left", 400, 1)
txt(40, 420, "THE MPC NEEDS A WIFI CONNECTION TO THE INTERNET. PRESS TEST TO CHECK IT.", 1.8, "b9a46a", "left", 400, 1)
btn("net_test", 220, 520, 300, 64, "test")
ro_("net_txt", 800, 520, 800, 40, 28, "f2e8cc", "left", "semibold")
txt(40, 600, "YOUR CRATE AND PADS ARE SAVED IN:", 1.6, "8a7440", "left", 400, 1)
ro_("dir_txt", 640, 636, 1200, 30, 22, "b9a46a", "left")
txt(1240, 676, "JJ DA PLUG  -  MADE BY JJ-LEMMING", 1.5, "8a7440", "right", 400, 2)
q()
open(OUT + "/layout.conf", "w").write("\n".join(L) + "\n")

json.dump({"name": "Crate Digga", "vendor": "JJ DA PLUG", "uid": "CrDg", "version": 100, "so": "crate.so", "params": "params.json",
           "layout": "layout.conf", "art": "html", "defines": {"PARAM_TEXT_MAX": 128, "HAS_DISPLAY_REV": 1},
           "build": {"root": ".", "sources": ["crate.c", "vorbis_glue.c"], "cflags": ["-I."], "libs": ["-lpthread", "-lm", "-ldl"]}},
          open(OUT + "/vst.json", "w"), indent=1)
print("params", len(P), "layout lines", len(L))
