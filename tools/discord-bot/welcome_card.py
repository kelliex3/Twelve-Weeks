"""#new-survivors welcome card: one picture per new member, in the look of the server's other bot art
(release-art/discord_art.py: charcoal, cream Rough title, DIN-like body text, thin white rules).

  render(name, avatar_png_bytes, number) -> PNG bytes      (used by the bot's on_member_join)
  python welcome_card.py bg <out.png>                       write the static background (needs numpy; run once, on the PC)
  python welcome_card.py demo <out_dir>                     write sample cards (sample avatars, long / non-Latin names)

The bot only needs Pillow + fontTools at run time. Files it reads from ./welcome-art:
  welcome-card-bg.png, fonts/rough.ttf (The Last Of Us Rough), fonts/body.ttf (Bahnschrift), fonts/fallback.ttf (DejaVu Sans Bold)."""
import io
import os
import sys

from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.join(HERE, "welcome-art")   # its own folder (the shared release-art belongs to the old repo)
W, H = 1920, 720
BG, CREAM, WHITE, GREY = (14, 15, 17), (214, 206, 184), (230, 230, 230), (150, 152, 156)
AV = 236                      # avatar diameter
AV_CY = 226                   # avatar centre row
NAME_Y = 480
RULE_Y = 578
FALLBACK_SYS = ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "C:/Windows/Fonts/seguisb.ttf")


def _font_path(name):
    return os.path.join(ART, "fonts", name)


def _cmap(path):
    try:
        from fontTools.ttLib import TTFont
        return set(TTFont(path, lazy=True).getBestCmap())
    except Exception:
        return None


_CACHE = {}


def _fonts():
    if not _CACHE:
        fb = _font_path("fallback.ttf")
        if not os.path.exists(fb):
            fb = next((p for p in FALLBACK_SYS if os.path.exists(p)), _font_path("body.ttf"))
        _CACHE.update(rough=_font_path("rough.ttf"), body=_font_path("body.ttf"), fallback=fb)
        _CACHE["cmap"] = {k: _cmap(_CACHE[k]) for k in ("rough", "body", "fallback")}
    return _CACHE


def body(size, weight="SemiLight", path=None):
    f = ImageFont.truetype(path or _fonts()["body"], size)
    try:
        f.set_variation_by_name(weight)
    except Exception:
        pass
    return f


def tracked(d, cx, y_mid, text, f, fill, track=0):
    """text centred on cx with letter spacing, its ink box vertically centred on y_mid"""
    widths = [d.textlength(c, font=f) for c in text]
    tw = sum(widths) + track * max(0, len(text) - 1)
    b = d.textbbox((0, 0), text, font=f)
    x, y = cx - tw / 2, y_mid - (b[1] + b[3]) / 2
    for c, cw in zip(text, widths):
        d.text((x, y), c, font=f, fill=fill)
        x += cw + track


def rule(d, x0, x1, y, a=110):
    d.line([(x0, y), (x1, y)], fill=(255, 255, 255, a), width=2)


def clean_name(name, fallback="survivor"):
    """display name for the card: drop characters no font here can draw (emoji, symbols), collapse spaces;
    when less than half of the letters survive (a CJK name with no font for it), the fallback (the ASCII username)"""
    cm = _fonts()["cmap"]
    ok = set()
    for k in ("rough", "body", "fallback"):
        if cm[k]:
            ok |= cm[k]
    s = "".join(c for c in name if c == " " or (ord(c) in ok if ok else c.isprintable()))
    s = " ".join(s.split())
    if len(s.replace(" ", "")) * 2 < len(name.replace(" ", "")) or not s:
        return fallback
    return s


def name_font(d, text, max_w):
    """the Rough title font when it has every letter, else Bahnschrift Bold, else DejaVu Sans Bold; shrunk to fit max_w"""
    fs, cm = _fonts(), _fonts()["cmap"]
    up = text.upper()
    if cm["rough"] and all(c == " " or ord(c) in cm["rough"] for c in up):
        path, size, minsize, txt, track = fs["rough"], 150, 70, up, 2
    elif cm["body"] and all(c == " " or ord(c) in cm["body"] for c in up):
        path, size, minsize, txt, track = fs["body"], 104, 52, up, 4
    else:
        path, size, minsize, txt, track = fs["fallback"], 96, 46, text, 2
    while True:
        f = body(size, "Bold", path) if path == fs["body"] else ImageFont.truetype(path, size)
        tw = sum(d.textlength(c, font=f) for c in txt) + track * max(0, len(txt) - 1)
        if tw <= max_w or size <= minsize:
            break
        size -= 4
    if tw > max_w:                       # still too wide at the smallest size: cut with an ellipsis
        while txt and tw > max_w:
            txt = txt[:-1]
            tw = sum(d.textlength(c, font=f) for c in txt + "...") + track * len(txt)
        txt = txt.rstrip() + "..."
    return f, txt, track


def avatar_disc(png, size):
    try:
        a = Image.open(io.BytesIO(png)).convert("RGBA")
    except Exception:
        a = Image.new("RGBA", (size, size), (40, 42, 46, 255))
    s = min(a.size)
    a = a.crop(((a.size[0] - s) // 2, (a.size[1] - s) // 2, (a.size[0] + s) // 2, (a.size[1] + s) // 2))
    k = 4                                            # supersampled mask = smooth edge
    a = a.resize((size * k, size * k), Image.LANCZOS)
    # a touch of the art's grade: slightly desaturated, warm
    grey = a.convert("L").convert("RGBA")
    a = Image.blend(a, grey, 0.18)
    m = Image.new("L", (size * k, size * k), 0)
    ImageDraw.Draw(m).ellipse([0, 0, size * k - 1, size * k - 1], fill=255)
    a.putalpha(m)
    return a.resize((size, size), Image.LANCZOS)


def render(name, avatar_png, number=None, username="survivor"):
    bgp = os.path.join(ART, "welcome-card-bg.png")
    im = Image.open(bgp).convert("RGBA") if os.path.exists(bgp) else Image.new("RGBA", (W, H), BG + (255,))
    d = ImageDraw.Draw(im, "RGBA")
    fs = _fonts()
    tracked(d, W / 2, 40, "TWELVE WEEKS", ImageFont.truetype(fs["rough"], 40), (214, 206, 184, 150))

    # avatar: soft dark shadow, the picture, a thin cream ring with a gap, like a dog tag frame
    cx, cy, r = W // 2, AV_CY, AV // 2
    sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse([cx - r - 6, cy - r + 4, cx + r + 6, cy + r + 16], fill=(0, 0, 0, 200))
    im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(14)))
    im.alpha_composite(avatar_disc(avatar_png, AV), (cx - r, cy - r))
    k = 4
    ring = Image.new("RGBA", ((AV + 44) * k, (AV + 44) * k), (0, 0, 0, 0))
    rd = ImageDraw.Draw(ring)
    rd.ellipse([8 * k, 8 * k, (AV + 36) * k, (AV + 36) * k], outline=CREAM + (235,), width=3 * k)
    rd.ellipse([0, 0, (AV + 44) * k - 1, (AV + 44) * k - 1], outline=(255, 255, 255, 40), width=1 * k)
    ring = ring.resize((AV + 44, AV + 44), Image.LANCZOS)
    im.alpha_composite(ring, (cx - r - 22, cy - r - 22))

    d = ImageDraw.Draw(im, "RGBA")
    f, txt, track = name_font(d, clean_name(name, username), W - 360)
    tracked(d, W / 2, NAME_Y, txt, f, CREAM, track=track)
    rule(d, W / 2 - 360, W / 2 + 360, RULE_Y)
    sub = f"NEW SURVIVOR  #{number:,}" if number else "NEW SURVIVOR"
    tracked(d, W / 2, (RULE_Y + H) / 2, sub, body(38), GREY, track=6)
    out = io.BytesIO()
    im.convert("RGB").save(out, "PNG", optimize=True)
    return out.getvalue()


def write_bg(path):
    """the charcoal ground of discord_art.canvas() at this card's size (numpy, PC only)"""
    import numpy as np
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    rr = np.hypot((xx - W / 2) / (W * 0.62), (yy - H * 0.45) / (H * 0.9))
    lift = np.clip(1 - rr, 0, 1) ** 2 * 14
    grain = np.random.default_rng(12).normal(0, 2.2, (H, W)).astype(np.float32)
    vig = np.clip(rr - 0.55, 0, 1) * 10
    a = np.array(BG, np.float32)[None, None, :] + (lift + grain - vig)[..., None]
    Image.fromarray(a.clip(0, 255).astype(np.uint8)).save(path, optimize=True)


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "bg":
        write_bg(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == "demo":
        out = sys.argv[2]
        os.makedirs(out, exist_ok=True)
        import random
        for i, (nm, col) in enumerate((("Abby", (120, 140, 160)), ("xX_ShamblerSlayer_Xx", (90, 60, 50)),
                                        ("Zofia Kowalska 🌲", (60, 80, 60)), ("Дмитрий", (70, 70, 90)), ("ゆうき", (110, 90, 70)))):
            av = Image.new("RGB", (256, 256), col)
            ImageDraw.Draw(av).ellipse([70, 40, 186, 156], fill=tuple(min(255, c + 70) for c in col))
            ImageDraw.Draw(av).ellipse([30, 150, 226, 330], fill=tuple(min(255, c + 50) for c in col))
            b = io.BytesIO(); av.save(b, "PNG")
            open(os.path.join(out, f"card-{i}.png"), "wb").write(render(nm, b.getvalue(), random.Random(i).randint(300, 2400), "yuuki_04"))
        print("demo cards in", out)
    else:
        sys.exit(__doc__)
