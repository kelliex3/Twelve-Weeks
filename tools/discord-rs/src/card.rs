use ab_glyph::{point, Font, FontVec, PxScale, ScaleFont, VariableFont};
use std::path::{Path, PathBuf};

const W: usize = 1920;
const H: usize = 720;
const BG: [u8; 3] = [14, 15, 17];
const CREAM: [u8; 3] = [214, 206, 184];
const GREY: [u8; 3] = [150, 152, 156];
const AV: usize = 236;
const AV_CY: usize = 226;
const NAME_Y: f32 = 480.0;
const RULE_Y: usize = 578;
const FALLBACK_SYS: &str = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
const MAX_AVATAR_BYTES: usize = 32 << 20;

pub struct Img {
    pub w: usize,
    pub h: usize,
    pub px: Vec<u8>,
}

impl Img {
    fn new(w: usize, h: usize, fill: [u8; 4]) -> Img {
        let mut px = Vec::with_capacity(w * h * 4);
        for _ in 0..w * h {
            px.extend_from_slice(&fill);
        }
        Img { w, h, px }
    }

    fn crop(&self, x0: usize, y0: usize, x1: usize, y1: usize) -> Img {
        let (w, h) = (x1 - x0, y1 - y0);
        let mut px = Vec::with_capacity(w * h * 4);
        for y in y0..y1 {
            px.extend_from_slice(&self.px[(y * self.w + x0) * 4..(y * self.w + x1) * 4]);
        }
        Img { w, h, px }
    }
}

pub fn decode_png(data: &[u8]) -> Result<Img, String> {
    let mut dec = png::Decoder::new_with_limits(std::io::Cursor::new(data), png::Limits { bytes: MAX_AVATAR_BYTES * 4 });
    dec.set_transformations(png::Transformations::normalize_to_color8());
    let mut r = dec.read_info().map_err(|e| e.to_string())?;
    let size = r.output_buffer_size().ok_or("png too large")?;
    let mut buf = vec![0; size];
    let info = r.next_frame(&mut buf).map_err(|e| e.to_string())?;
    let (w, h) = (info.width as usize, info.height as usize);
    if w == 0 || h == 0 {
        return Err("empty png".into());
    }
    let mut px = Vec::with_capacity(w * h * 4);
    let line = info.line_size;
    for y in 0..h {
        let row = &buf[y * line..];
        for x in 0..w {
            let p = match info.color_type {
                png::ColorType::Rgba => [row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3]],
                png::ColorType::Rgb => [row[x * 3], row[x * 3 + 1], row[x * 3 + 2], 255],
                png::ColorType::GrayscaleAlpha => [row[x * 2], row[x * 2], row[x * 2], row[x * 2 + 1]],
                png::ColorType::Grayscale => [row[x], row[x], row[x], 255],
                png::ColorType::Indexed => return Err("indexed png after expand".into()),
            };
            px.extend_from_slice(&p);
        }
    }
    Ok(Img { w, h, px })
}

pub fn encode_rgb(img: &Img) -> Result<Vec<u8>, String> {
    let mut rgb = Vec::with_capacity(img.w * img.h * 3);
    for p in img.px.chunks_exact(4) {
        rgb.extend_from_slice(&p[..3]);
    }
    let mut out = Vec::new();
    {
        let mut enc = png::Encoder::new(&mut out, img.w as u32, img.h as u32);
        enc.set_color(png::ColorType::Rgb);
        enc.set_depth(png::BitDepth::Eight);
        enc.set_compression(png::Compression::Balanced);
        let mut wr = enc.write_header().map_err(|e| e.to_string())?;
        wr.write_image_data(&rgb).map_err(|e| e.to_string())?;
    }
    Ok(out)
}

fn sinc(x: f64) -> f64 {
    if x == 0.0 {
        1.0
    } else {
        let x = x * std::f64::consts::PI;
        x.sin() / x
    }
}

fn lanczos(x: f64) -> f64 {
    if (-3.0..3.0).contains(&x) {
        sinc(x) * sinc(x / 3.0)
    } else {
        0.0
    }
}

const PREC: u32 = 22;

fn coeffs(in_size: usize, out_size: usize) -> (usize, Vec<(usize, usize)>, Vec<i64>) {
    let scale = in_size as f64 / out_size as f64;
    let filterscale = scale.max(1.0);
    let support = 3.0 * filterscale;
    let ksize = support.ceil() as usize * 2 + 1;
    let mut bounds = Vec::with_capacity(out_size);
    let mut kk = vec![0i64; out_size * ksize];
    for xx in 0..out_size {
        let center = (xx as f64 + 0.5) * scale;
        let ss = 1.0 / filterscale;
        let xmin = ((center - support + 0.5) as i64).max(0) as usize;
        let xmax = ((center + support + 0.5) as i64).min(in_size as i64) as usize;
        let n = xmax.saturating_sub(xmin);
        let mut k = vec![0f64; n];
        let mut ww = 0.0;
        for (x, kv) in k.iter_mut().enumerate() {
            let w = lanczos((x as f64 + xmin as f64 - center + 0.5) * ss);
            *kv = w;
            ww += w;
        }
        for (x, kv) in k.iter().enumerate() {
            let v = if ww != 0.0 { kv / ww } else { *kv };
            kk[xx * ksize + x] = if v < 0.0 { (-0.5 + v * f64::from(1u32 << PREC)) as i64 } else { (0.5 + v * f64::from(1u32 << PREC)) as i64 };
        }
        bounds.push((xmin, n));
    }
    (ksize, bounds, kk)
}

fn clip8(v: i64) -> u8 {
    (v >> PREC).clamp(0, 255) as u8
}

fn premultiply(img: &mut Img) {
    for p in img.px.chunks_exact_mut(4) {
        let a = u32::from(p[3]);
        for c in &mut p[..3] {
            let t = u32::from(*c) * a + 128;
            *c = (((t >> 8) + t) >> 8) as u8;
        }
    }
}

fn unpremultiply(img: &mut Img) {
    for p in img.px.chunks_exact_mut(4) {
        let a = u32::from(p[3]);
        if a != 0 && a != 255 {
            for c in &mut p[..3] {
                *c = (255 * u32::from(*c) / a).min(255) as u8;
            }
        }
    }
}

fn resize(src: &Img, dw: usize, dh: usize) -> Img {
    let mut s = Img { w: src.w, h: src.h, px: src.px.clone() };
    premultiply(&mut s);
    let tmp = if dw != s.w {
        let (ks, b, kk) = coeffs(s.w, dw);
        let mut out = vec![0u8; dw * s.h * 4];
        for y in 0..s.h {
            for (xx, &(xmin, n)) in b.iter().enumerate() {
                let mut acc = [1i64 << (PREC - 1); 4];
                for x in 0..n {
                    let k = kk[xx * ks + x];
                    let p = &s.px[(y * s.w + xmin + x) * 4..][..4];
                    for c in 0..4 {
                        acc[c] += i64::from(p[c]) * k;
                    }
                }
                for c in 0..4 {
                    out[(y * dw + xx) * 4 + c] = clip8(acc[c]);
                }
            }
        }
        Img { w: dw, h: s.h, px: out }
    } else {
        s
    };
    let mut fin = if dh != tmp.h {
        let (ks, b, kk) = coeffs(tmp.h, dh);
        let mut out = vec![0u8; dw * dh * 4];
        for (yy, &(ymin, n)) in b.iter().enumerate() {
            for x in 0..dw {
                let mut acc = [1i64 << (PREC - 1); 4];
                for y in 0..n {
                    let k = kk[yy * ks + y];
                    let p = &tmp.px[((ymin + y) * dw + x) * 4..][..4];
                    for c in 0..4 {
                        acc[c] += i64::from(p[c]) * k;
                    }
                }
                for c in 0..4 {
                    out[(yy * dw + x) * 4 + c] = clip8(acc[c]);
                }
            }
        }
        Img { w: dw, h: dh, px: out }
    } else {
        tmp
    };
    unpremultiply(&mut fin);
    fin
}

fn blur_radius(radius: f32, passes: f32) -> f32 {
    let sigma2 = radius * radius / passes;
    let l_big = (12.0 * sigma2 + 1.0).sqrt();
    let l = ((l_big - 1.0) / 2.0).floor();
    let a = (2.0 * l + 1.0) * (l * (l + 1.0) - 3.0 * sigma2) / (6.0 * (sigma2 - (l + 1.0) * (l + 1.0)));
    l + a
}

fn box_line(src: &[u8], out: &mut [u8], radius: f32) {
    let n = src.len() as i64;
    let ri = radius as i64;
    let ww = ((1u64 << 24) as f64 / (f64::from(radius) * 2.0 + 1.0)) as u64;
    let fw = ((1u64 << 24) - (ri as u64 * 2 + 1) * ww) / 2;
    let at = |i: i64| u64::from(src[i.clamp(0, n - 1) as usize]);
    for x in 0..n {
        let mut acc = 0u64;
        for k in -ri..=ri {
            acc += at(x + k);
        }
        let v = acc * ww + (at(x - ri - 1) + at(x + ri + 1)) * fw;
        out[x as usize] = ((v + (1 << 23)) >> 24).min(255) as u8;
    }
}

fn gaussian_blur(a: &mut [u8], w: usize, h: usize, radius: f32) {
    let r = blur_radius(radius, 3.0);
    let mut line = Vec::new();
    let mut out = Vec::new();
    for _ in 0..3 {
        for y in 0..h {
            line.clear();
            line.extend_from_slice(&a[y * w..(y + 1) * w]);
            out.resize(w, 0);
            box_line(&line, &mut out, r);
            a[y * w..(y + 1) * w].copy_from_slice(&out);
        }
    }
    for _ in 0..3 {
        for x in 0..w {
            line.clear();
            line.extend((0..h).map(|y| a[y * w + x]));
            out.resize(h, 0);
            box_line(&line, &mut out, r);
            for y in 0..h {
                a[y * w + x] = out[y];
            }
        }
    }
}

fn composite(dst: &mut Img, src: &Img, ox: usize, oy: usize) {
    for y in 0..src.h {
        let dy = oy + y;
        if dy >= dst.h {
            break;
        }
        for x in 0..src.w {
            let dx = ox + x;
            if dx >= dst.w {
                break;
            }
            let s = &src.px[(y * src.w + x) * 4..][..4];
            if s[3] == 0 {
                continue;
            }
            let d = &mut dst.px[(dy * dst.w + dx) * 4..][..4];
            let (sa, da) = (u32::from(s[3]), u32::from(d[3]));
            let blend = da * (255 - sa);
            let outa255 = sa * 255 + blend;
            let coef1 = sa * 255 * 255 * (1 << 7) / outa255;
            let coef2 = 255 * (1 << 7) - coef1;
            let div = |t: u32| (((t >> 8) + t) >> 8) >> 7;
            for c in 0..3 {
                let t = u32::from(s[c]) * coef1 + u32::from(d[c]) * coef2 + (0x80 << 7);
                d[c] = div(t) as u8;
            }
            let t = outa255 + 0x80;
            d[3] = (((t >> 8) + t) >> 8) as u8;
        }
    }
}

fn ellipse_mask(w: usize, h: usize, x0: f32, y0: f32, x1: f32, y1: f32, width: Option<f32>) -> Vec<u8> {
    let inside = |px: f32, py: f32, ax0: f32, ay0: f32, ax1: f32, ay1: f32| {
        let (cx, cy) = ((ax0 + ax1 + 1.0) / 2.0, (ay0 + ay1 + 1.0) / 2.0);
        let (rx, ry) = ((ax1 - ax0 + 1.0) / 2.0, (ay1 - ay0 + 1.0) / 2.0);
        if rx <= 0.0 || ry <= 0.0 {
            return false;
        }
        let (dx, dy) = ((px - cx) / rx, (py - cy) / ry);
        dx * dx + dy * dy <= 1.0
    };
    let mut m = vec![0u8; w * h];
    let ylo = y0.max(0.0) as usize;
    let yhi = ((y1 + 1.0).max(0.0) as usize).min(h);
    let xlo = x0.max(0.0) as usize;
    let xhi = ((x1 + 1.0).max(0.0) as usize).min(w);
    for y in ylo..yhi {
        for x in xlo..xhi {
            let (px, py) = (x as f32 + 0.5, y as f32 + 0.5);
            let mut on = inside(px, py, x0, y0, x1, y1);
            if let (true, Some(wd)) = (on, width) {
                on = !inside(px, py, x0 + wd, y0 + wd, x1 - wd, y1 - wd);
            }
            if on {
                m[y * w + x] = 255;
            }
        }
    }
    m
}

pub struct Fonts {
    rough: FontVec,
    semilight: FontVec,
    bold: FontVec,
    fallback: FontVec,
    fallback_is_body: bool,
}

#[derive(Clone, Copy, PartialEq)]
enum Which {
    Rough,
    Bold,
    Fallback,
}

impl Fonts {
    pub fn load(art: &Path) -> Result<Fonts, String> {
        let fonts = art.join("fonts");
        let read = |p: &Path| -> Result<FontVec, String> {
            let data = std::fs::read(p).map_err(|e| format!("{}: {e}", p.display()))?;
            FontVec::try_from_vec(data).map_err(|e| format!("{}: {e}", p.display()))
        };
        let body_path = fonts.join("body.ttf");
        let mut fb_path = fonts.join("fallback.ttf");
        if !fb_path.exists() {
            fb_path = if Path::new(FALLBACK_SYS).exists() { PathBuf::from(FALLBACK_SYS) } else { body_path.clone() };
        }
        let mut semilight = read(&body_path)?;
        semilight.set_variation(b"wght", 350.0);
        semilight.set_variation(b"wdth", 100.0);
        let mut bold = read(&body_path)?;
        bold.set_variation(b"wght", 700.0);
        bold.set_variation(b"wdth", 100.0);
        let fallback_is_body = fb_path == body_path;
        let fallback = read(&fb_path)?;
        Ok(Fonts { rough: read(&fonts.join("rough.ttf"))?, semilight, bold, fallback, fallback_is_body })
    }

    fn font(&self, w: Which) -> &FontVec {
        match w {
            Which::Rough => &self.rough,
            Which::Bold => &self.bold,
            Which::Fallback => &self.fallback,
        }
    }

    fn has(f: &FontVec, c: char) -> bool {
        f.glyph_id(c).0 != 0
    }

    pub fn clean_name(&self, name: &str, fallback: &str) -> String {
        let ok = |c: char| c == ' ' || Fonts::has(&self.rough, c) || Fonts::has(&self.semilight, c) || Fonts::has(&self.fallback, c);
        let s: String = name.chars().filter(|c| ok(*c)).collect();
        let s = s.split_whitespace().collect::<Vec<_>>().join(" ");
        let kept = s.chars().filter(|c| *c != ' ').count();
        let total = name.chars().filter(|c| *c != ' ').count();
        if kept * 2 < total || s.is_empty() {
            fallback.to_string()
        } else {
            s
        }
    }
}

struct Face<'a> {
    font: &'a FontVec,
    scale: PxScale,
}

impl<'a> Face<'a> {
    fn new(font: &'a FontVec, size: f32) -> Face<'a> {
        let upem = font.units_per_em().unwrap_or(1000.0);
        Face { font, scale: PxScale::from(size * font.height_unscaled() / upem) }
    }

    fn advance(&self, c: char) -> f32 {
        self.font.as_scaled(self.scale).h_advance(self.font.glyph_id(c))
    }

    fn width(&self, text: &str, track: f32) -> f32 {
        let n = text.chars().count();
        text.chars().map(|c| self.advance(c)).sum::<f32>() + track * n.saturating_sub(1) as f32
    }

    fn ink_rows(&self, text: &str) -> (f32, f32) {
        let mut lo = f32::MAX;
        let mut hi = f32::MIN;
        let mut x = 0.0;
        for c in text.chars() {
            let g = self.font.glyph_id(c).with_scale_and_position(self.scale, point(x, 0.0));
            if let Some(o) = self.font.outline_glyph(g) {
                let b = o.px_bounds();
                lo = lo.min(b.min.y);
                hi = hi.max(b.max.y);
            }
            x += self.advance(c);
        }
        if lo > hi {
            (0.0, 0.0)
        } else {
            (lo.floor(), hi.ceil())
        }
    }

    fn draw(&self, img: &mut Img, c: char, x: f32, baseline: f32, ink: [u8; 3]) {
        let g = self.font.glyph_id(c).with_scale_and_position(self.scale, point(x, baseline));
        let Some(o) = self.font.outline_glyph(g) else { return };
        let b = o.px_bounds();
        let (bx, by) = (b.min.x as i64, b.min.y as i64);
        let (w, h) = (img.w as i64, img.h as i64);
        o.draw(|gx, gy, cov| {
            let (px, py) = (bx + i64::from(gx), by + i64::from(gy));
            if px < 0 || py < 0 || px >= w || py >= h {
                return;
            }
            let m = (cov.clamp(0.0, 1.0) * 255.0).round() as u32;
            if m == 0 {
                return;
            }
            let d = &mut img.px[((py * w + px) * 4) as usize..][..4];
            let da = u32::from(d[3]);
            for (i, ch) in d[..3].iter_mut().enumerate() {
                let cm = 255 - (255 - m) * da / 255;
                let t = u32::from(*ch) * (255 - cm) + u32::from(ink[i]) * cm + 128;
                *ch = (((t >> 8) + t) >> 8) as u8;
            }
        });
    }
}

fn tracked(img: &mut Img, cx: f32, y_mid: f32, text: &str, face: &Face, ink: [u8; 3], track: f32) {
    let tw = face.width(text, track);
    let (lo, hi) = face.ink_rows(text);
    let baseline = y_mid - (lo + hi) / 2.0;
    let mut x = cx - tw / 2.0;
    for c in text.chars() {
        face.draw(img, c, x, baseline, ink);
        x += face.advance(c) + track;
    }
}

fn name_face<'a>(fonts: &'a Fonts, text: &str, max_w: f32) -> (Face<'a>, String, f32) {
    let up = text.to_uppercase();
    let all = |f: &FontVec, s: &str| s.chars().all(|c| c == ' ' || Fonts::has(f, c));
    let (which, mut size, min, txt, track) = if all(&fonts.rough, &up) {
        (Which::Rough, 150.0, 70.0, up, 2.0)
    } else if all(&fonts.semilight, &up) {
        (Which::Bold, 104.0, 52.0, up, 4.0)
    } else {
        (if fonts.fallback_is_body { Which::Bold } else { Which::Fallback }, 96.0, 46.0, text.to_string(), 2.0)
    };
    let mut face;
    let mut tw;
    loop {
        face = Face::new(fonts.font(which), size);
        tw = face.width(&txt, track);
        if tw <= max_w || size <= min {
            break;
        }
        size -= 4.0;
    }
    let mut chars: Vec<char> = txt.chars().collect();
    if tw > max_w {
        while !chars.is_empty() && tw > max_w {
            chars.pop();
            let s: String = chars.iter().collect::<String>() + "...";
            tw = s.chars().map(|c| face.advance(c)).sum::<f32>() + track * chars.len() as f32;
        }
        let s: String = chars.iter().collect();
        return (face, s.trim_end().to_string() + "...", track);
    }
    (face, txt, track)
}

fn thousands(n: u64) -> String {
    let s = n.to_string();
    let mut out = String::new();
    for (i, c) in s.chars().enumerate() {
        if i > 0 && (s.len() - i).is_multiple_of(3) {
            out.push(',');
        }
        out.push(c);
    }
    out
}

fn avatar_disc(png: &[u8], size: usize) -> Img {
    let a = decode_png(png).unwrap_or_else(|_| Img::new(size, size, [40, 42, 46, 255]));
    let s = a.w.min(a.h);
    let (x0, y0) = ((a.w - s) / 2, (a.h - s) / 2);
    let a = a.crop(x0, y0, x0 + s, y0 + s);
    let k = 4;
    let mut a = resize(&a, size * k, size * k);
    for p in a.px.chunks_exact_mut(4) {
        let l = ((u32::from(p[0]) * 19595 + u32::from(p[1]) * 38470 + u32::from(p[2]) * 7471 + 0x8000) >> 16) as u8;
        let g = [l, l, l, 255];
        for c in 0..4 {
            p[c] = (f32::from(p[c]) + 0.18f32 * (f32::from(g[c]) - f32::from(p[c]))) as u8;
        }
    }
    let n = size * k;
    let m = ellipse_mask(n, n, 0.0, 0.0, (n - 1) as f32, (n - 1) as f32, None);
    for (p, mv) in a.px.chunks_exact_mut(4).zip(m) {
        p[3] = mv;
    }
    resize(&a, size, size)
}

fn ring() -> Img {
    let k = 4;
    let n = (AV + 44) * k;
    let mut img = Img::new(n, n, [0, 0, 0, 0]);
    let outer = ellipse_mask(n, n, (8 * k) as f32, (8 * k) as f32, ((AV + 36) * k) as f32, ((AV + 36) * k) as f32, Some((3 * k) as f32));
    let edge = ellipse_mask(n, n, 0.0, 0.0, (n - 1) as f32, (n - 1) as f32, Some(k as f32));
    for (i, p) in img.px.chunks_exact_mut(4).enumerate() {
        if outer[i] != 0 {
            p.copy_from_slice(&[CREAM[0], CREAM[1], CREAM[2], 235]);
        }
        if edge[i] != 0 {
            p.copy_from_slice(&[255, 255, 255, 40]);
        }
    }
    resize(&img, AV + 44, AV + 44)
}

pub fn render(art: &Path, name: &str, avatar_png: &[u8], number: Option<u64>, username: &str) -> Result<Vec<u8>, String> {
    let fonts = Fonts::load(art)?;
    let bg = std::fs::read(art.join("welcome-card-bg.png")).ok().and_then(|d| decode_png(&d).ok());
    let mut im = match bg {
        Some(b) => b,
        None => Img::new(W, H, [BG[0], BG[1], BG[2], 255]),
    };
    let (w, h) = (im.w as f32, im.h as f32);
    tracked(&mut im, w / 2.0, 40.0, "TWELVE WEEKS", &Face::new(&fonts.rough, 40.0), CREAM, 0.0);

    let (cx, cy, r) = (im.w / 2, AV_CY, AV / 2);
    let margin = 60;
    let (sx0, sy0) = ((cx - r - 6).saturating_sub(margin), (cy + 4 - r).saturating_sub(margin));
    let (sx1, sy1) = ((cx + r + 6 + margin + 1).min(im.w), (cy + r + 16 + margin + 1).min(im.h));
    let (sw, sh) = (sx1 - sx0, sy1 - sy0);
    let mut sa = ellipse_mask(
        sw,
        sh,
        (cx - r - 6 - sx0) as f32,
        (cy + 4 - r - sy0) as f32,
        (cx + r + 6 - sx0) as f32,
        (cy + r + 16 - sy0) as f32,
        None,
    );
    for v in sa.iter_mut() {
        if *v != 0 {
            *v = 200;
        }
    }
    gaussian_blur(&mut sa, sw, sh, 14.0);
    let mut shadow = Img::new(sw, sh, [0, 0, 0, 0]);
    for (p, a) in shadow.px.chunks_exact_mut(4).zip(sa) {
        p[3] = a;
    }
    composite(&mut im, &shadow, sx0, sy0);
    composite(&mut im, &avatar_disc(avatar_png, AV), cx - r, cy - r);
    composite(&mut im, &ring(), cx - r - 22, cy - r - 22);

    let clean = fonts.clean_name(name, username);
    let (face, txt, track) = name_face(&fonts, &clean, w - 360.0);
    tracked(&mut im, w / 2.0, NAME_Y, &txt, &face, CREAM, track);
    let (x0, x1) = ((w / 2.0 - 360.0) as usize, (w / 2.0 + 360.0) as usize);
    for y in RULE_Y..RULE_Y + 2 {
        for x in x0..=x1.min(im.w - 1) {
            im.px[(y * im.w + x) * 4..][..4].copy_from_slice(&[255, 255, 255, 110]);
        }
    }
    let sub = match number.filter(|n| *n > 0) {
        Some(n) => format!("NEW SURVIVOR  #{}", thousands(n)),
        None => "NEW SURVIVOR".into(),
    };
    tracked(&mut im, w / 2.0, (RULE_Y as f32 + h) / 2.0, &sub, &Face::new(&fonts.semilight, 38.0), GREY, 6.0);
    encode_rgb(&im)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn thousands_sep() {
        assert_eq!(thousands(7), "7");
        assert_eq!(thousands(1234), "1,234");
        assert_eq!(thousands(1234567), "1,234,567");
    }

    #[test]
    fn blur_radius_like_pillow() {
        assert!((blur_radius(14.0, 3.0) - 13.482).abs() < 0.01);
    }

    #[test]
    fn resize_keeps_flat_color() {
        let src = Img::new(40, 30, [10, 200, 30, 255]);
        let out = resize(&src, 13, 7);
        assert!(out.px.chunks_exact(4).all(|p| p == [10, 200, 30, 255]));
    }

    #[test]
    fn png_roundtrip() {
        let img = Img::new(3, 2, [1, 2, 3, 255]);
        let back = decode_png(&encode_rgb(&img).unwrap()).unwrap();
        assert_eq!((back.w, back.h), (3, 2));
        assert_eq!(&back.px[..4], &[1, 2, 3, 255]);
    }
}
