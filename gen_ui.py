#!/usr/bin/env python3
"""Run by build.sh before the build. Generates params.json, layout.conf, vst.json and the skin images for MPC Messenger 1.4 (JJ DA PLUG)."""
import json, os, shutil
from PIL import Image
OUT = os.path.dirname(os.path.abspath(__file__))
IMG = OUT + "/images"
os.makedirs(IMG, exist_ok=True)
if os.path.exists(OUT + "/logo.png") and os.path.abspath(OUT + "/logo.png") != os.path.abspath(IMG + "/logo.png"):
    shutil.copy(OUT + "/logo.png", IMG + "/logo.png")
TABS = ["CHAT", "INBOX", "OUTBOX", "PADS", "HOST", "JOIN"]
QUICK = ["OK", "Ready", "On my way", "Thanks", "Check inbox", "Send file", "Call me", "Later"]
ROWS = 7
NTILE = 240          # the 1.3 tile parameters; 1.4 has 640 tiles (32 x 20): the other 400 are appended at the end
NTILE2 = 640
TW, TH_, TP = 32, 20, 15
VER = '1.4'

# gold palette (kept calm: dark charcoal, thin gold lines, cream text)
G1, G2, G3 = "#f0d98c", "#c9a24a", "#8a6a22"      # light, mid, deep gold
INK, PANEL, PANEL2, LINE = "#f2e8cc", "#17130d", "#241d10", "#6f5a2a"

# ------------------------------------------------------------------ params
P = []
def opt(key, name, options, default=0): P.append({"key": key, "name": name, "options": options, "default": default})
def ro(key, name): P.append({"key": key, "name": name, "type": "readout", "display": "string"})
def trig(key, name): P.append({"key": key, "name": name, "type": "trigger", "momentary": True})
opt("server", "Server", ["off", "on"])
opt("scan", "Search hosts", ["off", "on"])
opt("quick", "Quick text", QUICK)
opt("qr_show", "QR shown", ["off", "on"])
for k, n in [("hdr_txt", "Status"), ("note_txt", "Notice"), ("status", "Address"), ("code_txt", "Code"), ("online_txt", "Online"),
             ("name_txt", "This MPC"), ("in_page_txt", "Inbox page"), ("out_page_txt", "Outbox page"), ("b_page_txt", "Files page"),
             ("in_sel_txt", "Inbox selection"), ("out_sel_txt", "Outbox selection"), ("b_sel_txt", "Files selection"), ("b_path_txt", "Folder"),
             ("jcode_txt", "Join code"), ("cl_btn_txt", "Connect text"), ("cl_status", "Join status"), ("pic_txt", "Picture"),
             ("pad_info_txt", "Pad info"), ("in_pad_txt", "Inbox to pad")]: ro(k, n)
for i in range(1, ROWS + 1):
    ro("m%d" % i, "Chat line %d" % i); trig("ct%d" % i, "Chat line %d tap" % i)
    ro("i%d" % i, "Inbox row %d" % i); trig("it%d" % i, "Inbox row %d tap" % i)
    ro("o%d" % i, "Outbox row %d" % i); trig("ot%d" % i, "Outbox row %d tap" % i)
    ro("b%d" % i, "Files row %d" % i); trig("bt%d" % i, "Files row %d tap" % i)
for i in range(1, 5):
    ro("p%d" % i, "Host %d" % i); trig("pt%d" % i, "Host %d tap" % i)
for i in range(1, 17):
    ro("pn%d" % i, "Pad %d name" % i); trig("pd%d" % i, "Pad %d" % i)
for t, n in [("sendtxt", "Send text"), ("sendfile", "Send file"), ("newcode", "New code"),
             ("in_prev", "Inbox previous"), ("in_next", "Inbox next"), ("out_prev", "Outbox previous"), ("out_next", "Outbox next"),
             ("b_prev", "Files previous"), ("b_next", "Files next"), ("b_up", "Folder up"), ("b_out", "File to outbox"), ("b_pad", "File to pad"),
             ("in_pad", "Inbox to pad"), ("pad_load", "Load pad"), ("pad_clear", "Clear pad"),
             ("cl_btn", "Connect"), ("kp_back", "Keypad back")]: trig(t, n)
for d in range(10): trig("kp%d" % d, "Keypad %d" % d)
for i in range(100): P.append({"key": "q%d" % i, "name": "QR %d" % i, "min": 0.0, "max": 1.0, "default": 0.0})
for i in range(NTILE): P.append({"key": "t%d" % i, "name": "Tile %d" % i, "min": 0.0, "max": 1.0, "default": 0.0})
# added in 1.3: appended at the end so the saved parameter order of 1.2 stays the same
opt("obrowse", "Outbox view", ["list", "browse"])
ro("learn_txt", "Learn status")
for t, n in [("learn_pad", "Learn pad"), ("learn_base", "Learn base"), ("ob_on", "Outbox browse"), ("ob_off", "Outbox list")]: trig(t, n)
# ---- new in 1.4 (appended at the end, so the saved order of 1.3 stays)
for i in range(NTILE, NTILE2): P.append({"key": "t%d" % i, "name": "Tile %d" % i, "min": 0.0, "max": 1.0, "default": 0.0})
for k, n in [("last_txt", "Last MIDI note"), ("snd_pad_txt", "Sound pad"), ("att_txt", "Attack"), ("dec_txt", "Decay"), ("rel_txt", "Release"), ("grp_txt", "Mute group"),
             ("mode_txt", "Mode"), ("st_txt", "Start"), ("en_txt", "End"), ("pi_txt", "Pitch"), ("tp_txt", "Tempo"), ("sx_txt", "Stretch"), ("dir_txt", "Folder")]: ro(k, n)
for t, n in [("chat_clear", "Clear chat"), ("in_clear", "Clear inbox"), ("out_clear", "Clear outbox"), ("pad_prev", "Previous pad"), ("pad_next", "Next pad"), ("pad_test", "Play pad"),
             ("att_m", "Attack less"), ("att_p", "Attack more"), ("dec_m", "Decay less"), ("dec_p", "Decay more"), ("rel_m", "Release less"), ("rel_p", "Release more"),
             ("grp_m", "Group less"), ("grp_p", "Group more"), ("mode_t", "Mode switch"), ("st_m", "Start less"), ("st_p", "Start more"), ("en_m", "End less"), ("en_p", "End more"),
             ("pi_m", "Pitch down"), ("pi_p", "Pitch up"), ("tp_m", "Tempo less"), ("tp_p", "Tempo more"), ("sx_t", "Stretch switch")]: trig(t, n)
json.dump({"name": "MPC Messenger", "params": P}, open(OUT + "/params.json", "w"), indent=1)

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
    for sfx, w in (("", 1200), ("_n", 720), ("_p", 700), ("_m", 240)):
        svg("row" + sfx + ("_on" if on else ""), w, 48, '<rect x="1" y="1" width="%d" height="46" rx="9" fill="%s" stroke="%s" stroke-width="2"/>' % (w - 2, fill, stroke))
for nm, on in (("pad", False), ("pad_on", True)):
    top, bot, stroke = (("#f3dc90", "#b88f30", "#fff3c8") if on else ("#2b2312", "#18120a", LINE))
    svg(nm, 186, 84, "<defs>%s</defs>" % gradient("g", top, bot) + '<rect x="2" y="2" width="182" height="80" rx="10" fill="url(#g)" stroke="%s" stroke-width="2"/>' % stroke)
for nm, on in (("seg_off", False), ("seg_on", True)):
    fill, stroke = ((G2, "#fff3c8") if on else (PANEL2, LINE))
    svg(nm, 180, 44, '<rect x="1" y="1" width="178" height="42" rx="8" fill="%s" stroke="%s" stroke-width="2"/>' % (fill, stroke))
open(OUT + "/skin.css", "w").write(":root { --seg-size: 24px; --sheen: 0; --shadow: none; }\n.seg-tx { font-weight: 700; letter-spacing: 0.05em; }\n")
pair("sendtxt", 230, 84, "SEND", 40)
pair("sendfile", 380, 64, "SEND FILE", 36)
pair("sendfile_s", 260, 84, "SEND FILE", 32)
pair("newcode", 300, 70, "NEW CODE", 34, "dark")
pair("prev", 170, 56, "&#9666; PREV", 28, "dark")
pair("next", 170, 56, "NEXT &#9656;", 28, "dark")
pair("up", 150, 56, "&#9650; UP", 28, "dark")
pair("tooutbox", 330, 56, "COPY TO OUTBOX", 30)
pair("browse", 240, 56, "BROWSE", 30, "dark")
pair("done", 330, 56, "BACK TO OUTBOX", 28, "dark")
pair("learnpad", 380, 60, "LEARN THIS PAD", 28, "dark")
pair("learnbase", 380, 60, "LEARN ALL PADS", 28, "dark")
pair("clrchat", 230, 48, "CLEAR CHAT", 26, "dark")
pair("clrin", 200, 56, "CLEAR INBOX", 26, "dark")
pair("clrout", 220, 40, "CLEAR OUTBOX", 24, "dark")
pair("padprev", 170, 56, "&#9666; PAD", 28, "dark")
pair("padnext", 170, 56, "PAD &#9656;", 28, "dark")
pair("padtest", 200, 56, "PLAY PAD", 28)
pair("minus", 70, 52, "-", 40, "dark")
pair("plus", 70, 52, "+", 40, "dark")
pair("topad", 330, 56, "TO PAD", 30)
pair("inpad", 380, 64, "", 1)             # live text "TO PAD n"
pair("padload", 380, 60, "LOAD FROM INBOX", 30)
pair("padclear", 380, 60, "CLEAR PAD", 30, "dark")
pair("back", 150, 84, "&#9003;", 40, "dark")
for d in range(10): pair("kp%d" % d, 150, 84, str(d), 46, "dark")
pair("cl_btn", 360, 64, "", 1)             # text comes live (CONNECT / LEAVE)
# QR strip: frame f (0..511) draws the 3 x 3 modules whose bit r*3+c is set in f as dark squares, 8 px each
M = 8
strip = Image.new("L", (3 * M, 512 * 3 * M), 255)
px = strip.load()
for f in range(512):
    for r in range(3):
        for c in range(3):
            if f >> (r * 3 + c) & 1:
                for y in range(M):
                    for x in range(M):
                        px[c * M + x, f * 3 * M + r * M + y] = 0
strip.convert("RGB").save(IMG + "/qr_strip.png", optimize=True)
# picture strip: frame f (0..215) is one flat colour, r = f//36, g = f//6 % 6, b = f % 6, levels 0/51/../255; 15 x 15 px each
T = TP
pic = Image.new("RGB", (T, 216 * T))
for f in range(216):
    col = tuple((v * 51) for v in (f // 36, (f // 6) % 6, f % 6))
    pic.paste(col, (0, f * T, T, f * T + T))
pic.save(IMG + "/pic_strip.png", optimize=True)

# ------------------------------------------------------------------ layout
L = []
def add(s): L.append(s)
add("# MPC Messenger 1.4 - JJ DA PLUG. Eight native MPC tabs. Shadow coordinates (y = skin y + 86), plugin area 1280 x 628.")
add("art_css=skin.css\ntheme_bg=0a0805\ntheme_ink=f2e8cc\ntheme_accent=c9a24a\ntheme_line=6f5a2a\ntheme_seg_active_tx=1b1408")

def header():
    add("art file=images/bg.svg")
    add("art file=images/logo.png x=24 y=92 w=50 h=80")
    add('text cx=92 cy=122 label="MPC MESSENGER" size=4 color=f2e8cc align=left weight=700 spacing=3')
    add('text cx=94 cy=158 label="JJ DA PLUG" size=2 color=c9a24a align=left weight=600 spacing=6')
    add("readout box=0 w=560 h=40 cx=976 cy=124 label=\"\" key=hdr_txt tsize=36 tweight=bold tcolor=f0d98c talign=right tpad=0")
    add("readout box=0 w=560 h=30 cx=976 cy=158 label=\"\" key=name_txt tsize=22 tweight=regular tcolor=b9a46a talign=right tpad=0")

def rowbtn(key, get, cx, cy, w, h, img="row", size=34, when=""):
    add("button cx=%d cy=%d label=\"\" key=%s get=%s tsize=%d tweight=semibold tcolor=f2e8cc talign=left img=images/%s.svg img_on=images/%s_on.svg w=%d h=%d %s" % (cx, cy, key, get, size, img, img, w, h, when))
def btn(key, cx, cy, w, h, img=None, when=""):
    img = img or key
    add("button cx=%d cy=%d label=\"\" key=%s img=images/b_%s.svg img_on=images/b_%s_on.svg w=%d h=%d %s" % (cx, cy, key, img, img, w, h, when))
def ro_(key, cx, cy, w, h, size, color, align, weight="regular", when=""):
    add("readout box=0 w=%d h=%d cx=%d cy=%d label=\"\" key=%s tsize=%d tweight=%s tcolor=%s talign=%s tpad=0 %s" % (w, h, cx, cy, key, size, weight, color, align, when))
def txt(cx, cy, label, size, color, align, weight=600, spacing=2):
    add('text cx=%d cy=%d label="%s" size=%s color=%s align=%s weight=%d spacing=%d' % (cx, cy, label, size, color, align, weight, spacing))
def tab(name, qlinks="-,-,-,-"):
    add("\n[tab %s]" % name)
    header()
    return lambda: add('qlinks "%s" = %s' % (name, qlinks))

ROW0, PITCH = 222, 52     # first row centre and spacing of the 7 list rows

# ---- CHAT
q = tab("CHAT", "quick,-,-,-")
for i in range(ROWS): rowbtn("ct%d" % (i + 1), "m%d" % (i + 1), 640, ROW0 + i * PITCH, 1200, 48)
ro_("note_txt", 400, 580, 720, 30, 24, "f0d98c", "left", "semibold")
btn("chat_clear", 1110, 580, 230, 48, "clrchat")
add("enum_h cx=396 cy=640 label=\"\" key=quick sw=180 sh=44 rows=2 img=images/seg_off.svg img_on=images/seg_on.svg")
btn("sendtxt", 880, 650, 230, 84)
btn("sendfile", 1130, 650, 260, 84, "sendfile_s")
q()
# ---- INBOX (list + picture preview)
q = tab("INBOX")
for i in range(ROWS): rowbtn("it%d" % (i + 1), "i%d" % (i + 1), 390, ROW0 + i * PITCH, 720, 48, "row_n", 26)
add("art file=images/frame.svg x=762 y=210 w=496 h=304")
for i in range(NTILE2):
    gx, gy = i % TW, i // TW
    add("meter cx=%d cy=%d w=%d h=%d key=t%d strip=images/pic_strip.png frames=216" % (770 + gx * TP + 7, 212 + gy * TP + 7, TP, TP, i))
ro_("pic_txt", 1010, 538, 490, 30, 22, "b9a46a", "center")
ro_("in_sel_txt", 390, 582, 720, 34, 26, "f2e8cc", "left", "semibold")
ro_("in_page_txt", 1010, 582, 490, 34, 26, "b9a46a", "center")
ro_("note_txt", 640, 618, 1200, 28, 22, "f0d98c", "left", "semibold")
btn("in_clear", 335, 672, 200, 56, "clrin")
add("button cx=130 cy=672 label=\"\" key=in_prev img=images/b_prev.svg img_on=images/b_prev_on.svg w=170 h=56")
add("button cx=640 cy=672 label=\"\" key=in_pad get=in_pad_txt tsize=34 tweight=bold tcolor=1b1408 talign=center img=images/b_inpad.svg img_on=images/b_inpad_on.svg w=380 h=64")
add("button cx=1150 cy=672 label=\"\" key=in_next img=images/b_next.svg img_on=images/b_next_on.svg w=170 h=56")
q()
# ---- OUTBOX (the list of files to send, or BROWSE through the whole MPC and copy a file into it)
q = tab("OUTBOX")
OFF, ON = "when=obrowse:list", "when=obrowse:browse"
for i in range(ROWS): rowbtn("ot%d" % (i + 1), "o%d" % (i + 1), 640, ROW0 + i * PITCH, 1200, 48, when=OFF)
ro_("out_sel_txt", 520, 582, 920, 34, 26, "f2e8cc", "left", "semibold", OFF)
ro_("out_page_txt", 1130, 582, 300, 34, 26, "b9a46a", "right", "regular", OFF)
add("button cx=130 cy=672 label=\"\" key=out_prev img=images/b_prev.svg img_on=images/b_prev_on.svg w=170 h=56 " + OFF)
btn("ob_on", 380, 672, 240, 56, "browse", OFF)
btn("sendfile", 800, 672, 380, 64, "sendfile", OFF)
add("button cx=1150 cy=672 label=\"\" key=out_next img=images/b_next.svg img_on=images/b_next_on.svg w=170 h=56 " + OFF)
ro_("b_path_txt", 640, 214, 1200, 28, 22, "b9a46a", "left", "regular", ON)
for i in range(ROWS): rowbtn("bt%d" % (i + 1), "b%d" % (i + 1), 640, 254 + i * 46, 1200, 42, "row", 28, ON)
ro_("b_sel_txt", 520, 586, 940, 32, 24, "f2e8cc", "left", "semibold", ON)
ro_("b_page_txt", 1120, 586, 260, 32, 24, "b9a46a", "right", "regular", ON)
btn("b_up", 100, 672, 150, 56, "up", ON)
btn("b_prev", 270, 672, 170, 56, "prev", ON)
btn("b_next", 450, 672, 170, 56, "next", ON)
btn("b_out", 740, 672, 330, 56, "tooutbox", ON)
btn("ob_off", 1100, 672, 330, 56, "done", ON)
ro_("note_txt", 640, 620, 1200, 28, 22, "f0d98c", "left", "semibold")
btn("out_clear", 1130, 628, 220, 40, "clrout", OFF)
q()
# ---- PADS (16 pads, pad 1 bottom left like the hardware)
q = tab("PADS")
for r in range(4):
    for c in range(4):
        n = (3 - r) * 4 + c + 1
        add("button cx=%d cy=%d label=\"\" key=pd%d get=pn%d tsize=24 tweight=semibold tcolor=f2e8cc talign=center img=images/pad.svg img_on=images/pad_on.svg w=186 h=84" % (125 + c * 196, 246 + r * 96, n, n))
txt(900, 226, "SELECTED PAD", 1.9, "b9a46a", "left")
ro_("pad_info_txt", 1050, 262, 420, 36, 24, "f2e8cc", "center", "semibold")
btn("pad_load", 1050, 326, 380, 60, "padload")
btn("pad_clear", 1050, 396, 380, 60, "padclear")
btn("learn_pad", 1050, 466, 380, 60, "learnpad")
btn("learn_base", 1050, 536, 380, 60, "learnbase")
ro_("learn_txt", 1050, 596, 420, 30, 22, "f0d98c", "center", "semibold")
ro_("last_txt", 330, 612, 620, 30, 22, "f2e8cc", "left", "semibold")
ro_("note_txt", 640, 650, 1200, 28, 22, "f0d98c", "left", "semibold")
q()
# ---- SOUND: the sound of the selected pad
q = tab("SOUND")
btn("pad_prev", 120, 236, 170, 56, "padprev")
ro_("snd_pad_txt", 520, 236, 520, 40, 30, "f2e8cc", "center", "bold")
btn("pad_next", 920, 236, 170, 56, "padnext")
btn("pad_test", 1150, 236, 200, 56, "padtest")
ROWY = [318, 382, 446, 510, 574]
LEFT = [("ATTACK", "att"), ("DECAY", "dec"), ("RELEASE", "rel"), ("START", "st"), ("END", "en")]
RIGHT = [("PITCH", "pi"), ("TEMPO", "tp"), ("STRETCH", "sx"), ("GROUP", "grp"), ("MODE", "mode")]
for col, (X0, items) in enumerate(((40, LEFT), (660, RIGHT))):
    for (lab, k), y in zip(items, ROWY):
        txt(X0, y, lab, 2.1, "b9a46a", "left")
        if k in ("sx", "mode"):
            add("button cx=%d cy=%d label=\"\" key=%s get=%s_txt tsize=30 tweight=bold tcolor=f0d98c talign=center img=images/row_m.svg img_on=images/row_m_on.svg w=240 h=52" % (X0 + 380, y, "sx_t" if k == "sx" else "mode_t", k))
        else:
            btn(k + "_m", X0 + 260, y, 70, 52, "minus")
            ro_(k + "_txt", X0 + 380, y, 140, 44, 30, "f0d98c", "center", "bold")
            btn(k + "_p", X0 + 500, y, 70, 52, "plus")
txt(640, 624, "SAME GROUP NUMBER = THE PADS CUT EACH OTHER OFF", 1.6, "8a7440", "center", 400, 2)
ro_("note_txt", 640, 664, 1200, 28, 22, "f0d98c", "left", "semibold")
q()
# ---- HOST
q = tab("HOST")
txt(40, 236, "SERVER", 2.2, "b9a46a", "left")
add("enum_h cx=330 cy=268 label=\"\" key=server sw=150 sh=64 img=images/seg_off.svg img_on=images/seg_on.svg")
ro_("status", 480, 350, 900, 48, 38, "f2e8cc", "left", "bold")
ro_("code_txt", 480, 436, 900, 90, 78, "f0d98c", "left", "bold")
ro_("online_txt", 480, 514, 900, 40, 28, "b9a46a", "left")
btn("newcode", 190, 600, 300, 70, "newcode")
add("art file=images/white.svg x=936 y=214 w=304 h=304 when=qr_show:on")
for i in range(100):
    gx, gy = i % 10, i // 10
    add("meter cx=%d cy=%d w=24 h=24 key=q%d strip=images/qr_strip.png frames=512 when=qr_show:on" % (968 + gx * 24 + 12, 246 + gy * 24 + 12, i))
txt(1088, 552, "SCAN WITH THE PHONE CAMERA", 1.7, "b9a46a", "center")
txt(1088, 582, "TO OPEN THE CHAT", 1.7, "b9a46a", "center")
txt(1088, 676, "JJ DA PLUG  -  MADE BY JJ-LEMMING  -  V1.4", 1.5, "8a7440", "center", 400, 2)
q()
# ---- JOIN
q = tab("JOIN")
txt(40, 236, "SEARCH FOR HOSTS", 2.0, "b9a46a", "left")
add("enum_h cx=580 cy=240 label=\"\" key=scan sw=150 sh=50 img=images/seg_off.svg img_on=images/seg_on.svg")
for i in range(4): rowbtn("pt%d" % (i + 1), "p%d" % (i + 1), 380, 306 + i * 52, 700, 48, "row_p", 30)
ro_("cl_status", 380, 520, 700, 40, 30, "f0d98c", "left", "semibold")
ro_("jcode_txt", 380, 584, 700, 70, 64, "f0d98c", "left", "bold")
add("button cx=380 cy=664 label=\"\" key=cl_btn get=cl_btn_txt tsize=34 tweight=bold tcolor=1b1408 talign=center img=images/b_cl_btn.svg img_on=images/b_cl_btn_on.svg w=360 h=64")
for n, d in enumerate([1, 2, 3, 4, 5, 6, 7, 8, 9]):
    btn("kp%d" % d, 845 + (n % 3) * 162, 262 + (n // 3) * 94, 150, 84)
btn("kp_back", 845, 544, 150, 84, "back")
btn("kp0", 1007, 544, 150, 84)
q()
# ---- INFO (last tab)
q = tab("INFO")
add("art file=images/logo.png x=40 y=206 w=150 h=240")
txt(250, 226, "MPC MESSENGER  " + VER, 3.0, "f0d98c", "left", 700, 3)
txt(250, 262, "BY JJ-LEMMING  -  JJ DA PLUG", 2.0, "f2e8cc", "left", 600, 3)
lines = [("HOST", "SERVER ON, THEN SCAN THE QR CODE WITH THE PHONE CAMERA. SAME WIFI ONLY."),
         ("CHAT / INBOX / OUTBOX", "TEXT AND FILES BOTH WAYS. CLEAR MOVES THE FILES TO A FOLDER CALLED _cleared."),
         ("PICTURES", "SHOWN AS 32 X 20 TILES (THE MPC'S LIMIT). THE FULL PICTURE STAYS IN THE INBOX."),
         ("PADS", "PAD 1-16 = MIDI NOTES 36-51 (ALSO 0-15, 48-63, 60-75, 72-87). NOTHING TO LEARN."),
         ("SOUND", "ATTACK, DECAY, RELEASE, START, END, PITCH, TEMPO, STRETCH AND MUTE GROUP PER PAD."),
         ("TRANSPORT", "STOP ON THE MPC LETS THE PADS GO. THE SEQUENCE PLAYS THEM AGAIN ON PLAY."),
         ("SECURITY", "NO INTERNET, NO CLOUD. A CODE FOR EVERY ROOM, ONLY YOUR OWN NETWORK.")]
y = 296
for head, body in lines:
    txt(250, y, head, 1.8, "c9a24a", "left", 700, 3)
    txt(250, y + 22, body, 1.5, "f2e8cc", "left", 400, 1)
    y += 50
txt(250, 676, "DATA FOLDER:", 1.5, "8a7440", "left", 600, 2)
ro_("dir_txt", 820, 676, 800, 28, 20, "b9a46a", "left")
q()
open(OUT + "/layout.conf", "w").write("\n".join(L) + "\n")

json.dump({"name": "MPC Messenger", "vendor": "JJ DA PLUG", "uid": "LnMg", "version": 140, "so": "lanmsg.so", "params": "params.json",
           "layout": "layout.conf", "art": "html", "defines": {"PARAM_TEXT_MAX": 128, "HAS_DISPLAY_REV": 1, "HAS_SONG_POS": 1},
           "build": {"root": ".", "sources": ["lanmsg.c"], "cflags": ["-I."], "libs": ["-lpthread", "-lm"]}},
          open(OUT + "/vst.json", "w"), indent=1)
print("params", len(P), "layout lines", len(L))
