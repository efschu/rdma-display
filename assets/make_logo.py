#!/usr/bin/env python3
# Builds the rdma-display logo (assets/logo-*.svg, assets/icon-*.svg): a memory chip, data packets on the
# link and a screen whose picture they light up directly (remote direct memory access), plus the wordmark
# set in Inter as paths. Needs fontTools (pip install fonttools) and fonts-inter.
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
    """the sign in a 160x96 box: the host's memory (a chip) writes over the link straight into the
    client's screen - remote direct memory access, no stop on the way"""
    sw = 6.5
    ink = t["ink"]
    pins = " ".join(f"M{x} 18.5 V22 M{x} 60 V63.5" for x in (14, 21, 28))
    # screen (92..156 x 18..62), open on the left between y 28 and 54, a short foot below
    frame = "M92 28 V25 Q92 18 99 18 H149 Q156 18 156 25 V55 Q156 62 149 62 H99 Q92 62 92 55 V54"
    stand = "M124 62 V73 M113 73 H135"
    g = [f'<g transform="translate({ox} {oy}) scale({k})">',
         f'<rect x="6" y="26" width="30" height="30" rx="5" fill="none" stroke="{ink}" stroke-width="5"/>',
         f'<path d="{pins}" fill="none" stroke="{ink}" stroke-width="4" stroke-linecap="round"/>',
         '<rect x="14" y="34" width="14" height="14" rx="2.5" fill="url(#lane)"/>',
         # the picture the lanes write: a lit panel inside the screen
         '<rect x="101" y="25" width="48" height="30" rx="3" fill="url(#lane)" opacity="0.9"/>',
         f'<path d="{frame} {stand}" fill="none" stroke="{ink}" stroke-width="{sw}" stroke-linecap="round" stroke-linejoin="round"/>']
    for y in (35, 41, 47):
        # on the wire: packets; through the opening: a solid run into the picture
        g.append(f'<line x1="43" y1="{y}" x2="80" y2="{y}" stroke="url(#lane)" stroke-width="4.2" stroke-linecap="round" stroke-dasharray="7 6"/>')
        g.append(f'<line x1="88" y1="{y}" x2="102" y2="{y}" stroke="url(#lane)" stroke-width="4.2" stroke-linecap="round"/>')
    g.append("</g>")
    return "\n".join(g)


def defs(t, x1, x2):
    return (f'<defs><linearGradient id="lane" gradientUnits="userSpaceOnUse" x1="{x1}" y1="0" x2="{x2}" y2="0">'
            f'<stop offset="0" stop-color="{t["a1"]}"/>'
            f'<stop offset="1" stop-color="{t["a2"]}"/></linearGradient></defs>')


def full(theme):
    t = THEMES[theme]
    H = 96
    k = 1.0
    size = 46
    base = 64
    p1, x = text_path(FONT_B, "rdma", size, 180, base, -0.01)
    hx, hw, hh = x + size * 0.09, size * 0.30, size * 0.085   # the hyphen: a short bar in the lanes' colours
    hy = base - size * 0.30
    p2, x2 = text_path(FONT_B, "display", size, hx + hw + size * 0.09, base, -0.01)
    W = int(x2 + 8)
    bar = (f'<linearGradient id="hy" gradientUnits="userSpaceOnUse" x1="{hx:.1f}" y1="0" x2="{hx + hw:.1f}" y2="0">'
           f'<stop offset="0" stop-color="{t["a1"]}"/><stop offset="1" stop-color="{t["a2"]}"/></linearGradient>')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W * 4 // 3}" height="{H * 4 // 3}" role="img" aria-label="rdma-display">\n'
            f'<title>rdma-display</title>\n{defs(t, 14, 150)}\n<defs>{bar}</defs>\n{mark(t, 0, 3)}\n'
            f'<path d="{p1}" fill="{t["ink"]}"/>\n'
            f'<rect x="{hx:.1f}" y="{hy - hh / 2:.1f}" width="{hw:.1f}" height="{hh:.1f}" rx="{hh / 2:.1f}" fill="url(#hy)"/>\n'
            f'<path d="{p2}" fill="{t["ink"]}"/>\n</svg>\n')


def icon(theme, bg=False):
    t = THEMES[theme]
    back = f'<rect width="160" height="160" rx="32" fill="{"#0B1220" if theme == "dark" else "#FFFFFF"}"/>' if bg else ""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 160 160" width="256" height="256" role="img" aria-label="rdma-display">\n'
            f'<title>rdma-display</title>\n{defs(t, 14, 150)}\n{back}\n{mark(t, 0, 34)}\n</svg>\n')


for th in THEMES:
    open(f"{OUT}/logo-{th}.svg", "w").write(full(th))
    open(f"{OUT}/icon-{th}.svg", "w").write(icon(th, bg=True))
print("ok")
