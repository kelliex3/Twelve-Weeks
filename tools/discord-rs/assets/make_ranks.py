"""All 10 rank symbols (our rebuilt chalk glyphs, re/win-screen-1112/src/rankglyph.py) with rank number + title, for the Discord /rank command."""
import os, sys
sys.path.insert(0, r"E:\ClaudeTLOU\TLOU2MP Revival\re\win-screen-1112\src")
os.chdir(r"E:\ClaudeTLOU\TLOU2MP Revival\re\win-screen-1112\src")
import rankglyph as RG
from PIL import Image, ImageDraw, ImageFont

NAMES = ("Fresh", "Rookie", "Scrapper", "Survivor", "Blooded", "Seasoned", "Veteran", "Hunter", "Hardened", "Ghost")
FONT = r"E:\ClaudeTLOU\TLOU2MP Revival\The Last Of Us Rough.ttf"
BG = (20, 20, 20)
CW, CH, TOP = 300, 330, 120
W, H = CW * 5, TOP + CH * 2 + 40
S = Image.new("RGBA", (W, H), BG + (255,)); d = ImageDraw.Draw(S)
ft = ImageFont.truetype(FONT, 78); fn = ImageFont.truetype(FONT, 40); fs = ImageFont.truetype(FONT, 30)
d.text((W // 2, 64), "RANKS", font=ft, fill=(232, 224, 204), anchor="mm")
for i in range(10):
    g = RG.Glyph(i, 200).image()
    cx, y0 = CW * (i % 5) + CW // 2, TOP + CH * (i // 5)
    box = 220
    if g.width > box or g.height > box:
        k = box / max(g.width, g.height); g = g.resize((int(g.width * k), int(g.height * k)), Image.LANCZOS)
    S.alpha_composite(g, (cx - g.width // 2, y0 + 115 - g.height // 2))
    d.text((cx, y0 + 250), "RANK %d" % (i + 1), font=fs, fill=(150, 145, 132), anchor="mm")
    d.text((cx, y0 + 292), NAMES[i].upper(), font=fn, fill=(232, 224, 204), anchor="mm")
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ranks.png")
S.convert("RGB").save(out, optimize=True)
print(out, S.size, os.path.getsize(out))
