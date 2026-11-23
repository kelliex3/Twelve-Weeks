import io
import json
import os
import sys

from PIL import Image, ImageDraw

CASES = [
    ("Abby", "a0.png", 1234, "abby"),
    ("xX_ShamblerSlayer_Xx", "a1.png", 87, "shambler"),
    ("Zofia Kowalska \U0001F332", "a2.png", 2400, "zofia"),
    ("Дмитрий", "a3.png", 301, "dmitry"),
    ("ゆうき", "a4.png", 999, "yuuki_04"),
    ("A display name that is far too long to ever fit on one card line", "a5.png", 1000000, "longname"),
    ("straße & Co", "wide.png", 5, "strasse"),
    ("Ellie", "-", 0, "ellie"),
]


def avatars(out):
    for i, col in enumerate(((120, 140, 160), (90, 60, 50), (60, 80, 60), (70, 70, 90), (110, 90, 70), (200, 40, 40))):
        av = Image.new("RGBA" if i % 2 else "RGB", (256, 256), col)
        d = ImageDraw.Draw(av)
        d.ellipse([70, 40, 186, 156], fill=tuple(min(255, c + 70) for c in col))
        d.ellipse([30, 150, 226, 330], fill=tuple(min(255, c + 50) for c in col))
        d.line([(0, 0), (255, 255)], fill=(255, 255, 255), width=5)
        av.save(os.path.join(out, "a%d.png" % i))
    wide = Image.new("RGB", (300, 180), (30, 30, 30))
    ImageDraw.Draw(wide).rectangle([100, 20, 200, 160], fill=(240, 200, 20))
    wide.save(os.path.join(out, "wide.png"))


def reference(bot_dir, out):
    sys.path.insert(0, bot_dir)
    import welcome_card
    for i, (name, av, num, user) in enumerate(CASES):
        data = open(os.path.join(out, av), "rb").read() if av != "-" else b""
        png = welcome_card.render(name, data, num or None, user)
        open(os.path.join(out, "ref-%d.png" % i), "wb").write(png)


def compare(out):
    from PIL import ImageChops
    rows = []
    for i, case in enumerate(CASES):
        a = Image.open(os.path.join(out, "ref-%d.png" % i)).convert("RGB")
        b = Image.open(os.path.join(out, "rs-%d.png" % i)).convert("RGB")
        if a.size != b.size:
            rows.append({"case": i, "name": case[0], "error": "size %s vs %s" % (a.size, b.size)})
            continue
        diff = ImageChops.difference(a, b).convert("L")
        hist = diff.histogram()
        n = a.size[0] * a.size[1]
        mean = sum(v * c for v, c in enumerate(hist)) / n
        mse = sum(v * v * c for v, c in enumerate(hist)) / n
        over16 = sum(hist[17:]) / n * 100
        over64 = sum(hist[65:]) / n * 100
        psnr = 99.0 if mse == 0 else 10 * __import__("math").log10(255 * 255 / mse)
        rows.append({"case": i, "name": case[0], "mean_abs": round(mean, 3), "psnr_db": round(psnr, 2),
                     "pct_px_diff_gt16": round(over16, 3), "pct_px_diff_gt64": round(over64, 3), "max": max(v for v, c in enumerate(hist) if c)})
        side = Image.new("RGB", (a.size[0], a.size[1] * 3))
        side.paste(a, (0, 0))
        side.paste(b, (0, a.size[1]))
        side.paste(ImageChops.difference(a, b).point(lambda v: min(255, v * 4)), (0, a.size[1] * 2))
        side.resize((a.size[0] // 2, a.size[1] * 3 // 2), Image.LANCZOS).save(os.path.join(out, "side-%d.png" % i))
    print(json.dumps(rows, indent=1, ensure_ascii=False))


if __name__ == "__main__":
    cmd, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    if cmd == "avatars":
        avatars(out)
    elif cmd == "reference":
        reference(sys.argv[3], out)
    elif cmd == "cases":
        print(json.dumps(CASES, ensure_ascii=False))
    elif cmd == "compare":
        compare(out)
