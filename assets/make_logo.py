#!/usr/bin/env python3
# Builds the rdma-display logo (assets/logo-*.svg, assets/icon-*.svg): a display frame, open on one side, with three data lanes running
# straight into it (memory written directly into the screen), plus the wordmark set in Inter as paths.
import sys
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
FONT_B = "/usr/share/fonts/opentype/inter/InterDisplay-SemiBold.otf"   # fonts-inter (SIL OFL)
FONT_R = "/usr/share/fonts/opentype/inter/InterDisplay-Medium.otf"

THEMES = {
    "light": {"ink": "#0F172A", "muted": "#64748B", "a1": "#0EA5E9", "a2": "#6366F1"},
    "dark":  {"ink": "#F1F5F9", "muted": "#94A3B8", "a1": "#38BDF8", "a2": "#818CF8"},
}


def text_path(font_file, text, size, x0, baseline, tracking=0.0):
    """the text as one SVG path (y down), and the x after it"""
    f = TTFont(font_file)
    gs, cmap = f.getGlyphSet(), f.getBestCmap()
    upm = f["head"].unitsPerEm
    s = size / upm
    pen = SVGPathPen(gs)
    x = x0
    for ch in text:
        g = cmap[ord(ch)]
        tp = TransformPen(pen, (s, 0, 0, -s, x, baseline))
        gs[g].draw(tp)
        x += gs[g].width * s + tracking * size
    return pen.getCommands(), x - tracking * size


def mark(t, ox=0, oy=0, k=1.0):
    """the sign in a 96x96 box, placed at (ox, oy) scaled by k"""
    sw = 6.5
    # screen 16:10 (26..92 x 18..60), open on the left between y 27 and 51, a short foot below
    frame = ("M26 27 V25 Q26 18 33 18 H85 Q92 18 92 25 V60 Q92 67 85 67 H33 Q26 67 26 60 V53")
    foot = "M48 79 H70"
    stem = "M59 67 V79"
    lanes = [(33, 6, 72), (40, 0, 82), (47, 12, 64)]           # y, x start, x end
    g = [f'<g transform="translate({ox} {oy}) scale({k})">',
         f'<path d="{frame}" fill="none" stroke="{t["ink"]}" stroke-width="{sw}" stroke-linecap="round" stroke-linejoin="round"/>',
         f'<path d="{stem} {foot}" fill="none" stroke="{t["ink"]}" stroke-width="{sw}" stroke-linecap="round" stroke-linejoin="round"/>']
    for y, x1, x2 in lanes:
        g.append(f'<line x1="{x1}" y1="{y}" x2="{x2}" y2="{y}" stroke="url(#lane)" stroke-width="4.2" stroke-linecap="round"/>')
    g.append("</g>")
    return "\n".join(g)


def defs(t, x1, x2):
    return (f'<defs><linearGradient id="lane" gradientUnits="userSpaceOnUse" x1="{x1}" y1="0" x2="{x2}" y2="0">'
            f'<stop offset="0" stop-color="{t["a1"]}" stop-opacity="0"/>'
            f'<stop offset="0.45" stop-color="{t["a1"]}"/>'
            f'<stop offset="1" stop-color="{t["a2"]}"/></linearGradient></defs>')


def full(theme):
    t = THEMES[theme]
    H = 96
    k = 1.0
    size = 46
    base = 64
    p1, x = text_path(FONT_B, "rdma", size, 114, base, -0.01)
    hx, hw, hh = x + size * 0.09, size * 0.30, size * 0.085   # the hyphen: a short bar in the lanes' colours
    hy = base - size * 0.30
    p2, x2 = text_path(FONT_R, "display", size, hx + hw + size * 0.09, base, -0.01)
    W = int(x2 + 8)
    bar = (f'<linearGradient id="hy" gradientUnits="userSpaceOnUse" x1="{hx:.1f}" y1="0" x2="{hx + hw:.1f}" y2="0">'
           f'<stop offset="0" stop-color="{t["a1"]}"/><stop offset="1" stop-color="{t["a2"]}"/></linearGradient>')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W * 4 // 3}" height="{H * 4 // 3}" role="img" aria-label="rdma-display">\n'
            f'<title>rdma-display</title>\n{defs(t, 0, 82)}\n<defs>{bar}</defs>\n{mark(t)}\n'
            f'<path d="{p1}" fill="{t["ink"]}"/>\n'
            f'<rect x="{hx:.1f}" y="{hy - hh / 2:.1f}" width="{hw:.1f}" height="{hh:.1f}" rx="{hh / 2:.1f}" fill="url(#hy)"/>\n'
            f'<path d="{p2}" fill="{t["muted"]}"/>\n</svg>\n')


def icon(theme, bg=False):
    t = THEMES[theme]
    back = f'<rect width="96" height="96" rx="20" fill="{"#0B1220" if theme == "dark" else "#FFFFFF"}"/>' if bg else ""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 96 96" width="256" height="256" role="img" aria-label="rdma-display">\n'
            f'<title>rdma-display</title>\n{defs(t, 0, 82)}\n{back}\n{mark(t)}\n</svg>\n')


for th in THEMES:
    open(f"{OUT}/logo-{th}.svg", "w").write(full(th))
    open(f"{OUT}/icon-{th}.svg", "w").write(icon(th, bg=True))
print("ok")
