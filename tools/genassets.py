#!/usr/bin/env python3
"""Generate ReverticalOS visual assets.

Everything here is baked into a compact binary blob (QOI images + font
atlases) which the kernel decodes at boot.  The wallpaper is derived from the
supplied design reference, with the UI baked into the photo removed first.
"""
import math
import struct
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
PREVIEW = BUILD / "preview"
REFERENCE = ROOT / "reverticalos_ui.jpeg"
FONT_DIR = ROOT / "tools" / "fonts"

# ---------------------------------------------------------------------------
# QOI encoder (Quite Ok Image format)
# ---------------------------------------------------------------------------
def qoi_encode(img: Image.Image) -> bytes:
    img = img.convert("RGBA")
    w, h = img.size
    data = img.tobytes()
    out = bytearray(b"qoif" + struct.pack(">IIBB", w, h, 4, 0))
    index = [(0, 0, 0, 0)] * 64
    prev = (0, 0, 0, 255)
    run = 0
    n = len(data)
    for i in range(0, n, 4):
        px = (data[i], data[i + 1], data[i + 2], data[i + 3])
        if px == prev:
            run += 1
            if run == 62 or i + 4 == n:
                out.append(0xC0 | (run - 1))
                run = 0
            continue
        if run:
            out.append(0xC0 | (run - 1))
            run = 0
        idx = (px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) & 63
        if index[idx] == px:
            out.append(idx)
        else:
            index[idx] = px
            if px[3] == prev[3]:
                dr = px[0] - prev[0]
                dg = px[1] - prev[1]
                db = px[2] - prev[2]
                if -2 <= dr <= 1 and -2 <= dg <= 1 and -2 <= db <= 1:
                    out.append(0x40 | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2))
                else:
                    dr_dg = dr - dg
                    db_dg = db - dg
                    if -8 <= dr_dg <= 7 and -8 <= dg <= 7 and -8 <= db_dg <= 7:
                        out.append(0x80 | (dg + 32))
                        out.append(((dr_dg + 8) << 4) | (db_dg + 8))
                    else:
                        out += bytes((0xFE, px[0], px[1], px[2]))
            else:
                out += bytes((0xFF, px[0], px[1], px[2], px[3]))
        prev = px
    out += b"\0\0\0\0\0\0\0\1"
    return bytes(out)


def qoi_decode(blob: bytes) -> Image.Image:
    assert blob[:4] == b"qoif"
    w, h, channels, _ = struct.unpack(">IIBB", blob[4:14])
    px = bytearray(w * h * 4)
    index = [(0, 0, 0, 0)] * 64
    r = g = b = 0
    a = 255
    pos = 14
    p = 0
    run = 0
    total = w * h
    while p < total:
        if run:
            run -= 1
        else:
            op = blob[pos]
            pos += 1
            if op == 0xFE:
                r, g, b = blob[pos], blob[pos + 1], blob[pos + 2]
                pos += 3
            elif op == 0xFF:
                r, g, b, a = blob[pos], blob[pos + 1], blob[pos + 2], blob[pos + 3]
                pos += 4
            elif op & 0xC0 == 0x00:
                r, g, b, a = index[op]
            elif op & 0xC0 == 0x40:
                r = (r + ((op >> 4) & 3) - 2) & 255
                g = (g + ((op >> 2) & 3) - 2) & 255
                b = (b + (op & 3) - 2) & 255
            elif op & 0xC0 == 0x80:
                dg = (op & 0x3F) - 32
                op2 = blob[pos]
                pos += 1
                r = (r + dg + ((op2 >> 4) & 0xF) - 8) & 255
                g = (g + dg) & 255
                b = (b + dg + (op2 & 0xF) - 8) & 255
            else:
                run = op & 0x3F
            index[(r * 3 + g * 5 + b * 7 + a * 11) & 63] = (r, g, b, a)
        px[p * 4:p * 4 + 4] = bytes((r, g, b, a))
        p += 1
    return Image.frombytes("RGBA", (w, h), bytes(px))


# ---------------------------------------------------------------------------
# Colour helpers
# ---------------------------------------------------------------------------
def clamp(v, lo=0, hi=255):
    return max(lo, min(hi, int(v)))


def lerp(a, b, t):
    return a + (b - a) * t


def mix(c0, c1, t):
    return tuple(clamp(lerp(c0[i], c1[i], t)) for i in range(len(c0)))


def alpha(color, a):
    return (color[0], color[1], color[2], clamp(a))


def hx(s):
    s = s.lstrip("#")
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16), 255)


def linear_gradient(size, c0, c1, p0, p1):
    """Gradient image along the vector p0 -> p1."""
    w, h = size
    img = Image.new("RGBA", size)
    d = ImageDraw.Draw(img)
    vx, vy = p1[0] - p0[0], p1[1] - p0[1]
    denom = vx * vx + vy * vy or 1
    c0 = c0 if len(c0) == 4 else (*c0, 255)
    c1 = c1 if len(c1) == 4 else (*c1, 255)
    for y in range(h):
        for x in range(w):
            t = ((x - p0[0]) * vx + (y - p0[1]) * vy) / denom
            t = max(0.0, min(1.0, t))
            d.point((x, y), mix(c0, c1, t))
    return img


def radial_gradient(size, inner, outer, center=None, radius=None):
    w, h = size
    cx, cy = center if center else (w / 2, h / 2)
    rad = radius if radius else max(w, h) / 2
    img = Image.new("RGBA", size)
    d = ImageDraw.Draw(img)
    inner = inner if len(inner) == 4 else (*inner, 255)
    outer = outer if len(outer) == 4 else (*outer, 255)
    for y in range(h):
        for x in range(w):
            dist = math.hypot(x - cx, y - cy) / rad
            t = max(0.0, min(1.0, dist))
            d.point((x, y), mix(inner, outer, t ** 0.85))
    return img


def paste_masked(base, layer, mask):
    base.paste(layer, (0, 0), mask)


def rounded_mask(size, box, radius):
    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=radius, fill=255)
    return mask


# ---------------------------------------------------------------------------
# Wallpaper
# ---------------------------------------------------------------------------
def inpaint_reference(img: Image.Image) -> Image.Image:
    """Remove the baked-in UI from the reference image."""
    img = img.convert("RGB").copy()
    w, h = img.size
    px = img.load()

    # --- bottom-right widget stack -----------------------------------------
    # Continue the horizontal colour gradient from the clean strip to its left
    # out to the screen edge.  The result is a smooth field that blends into
    # the wallpaper and sits behind the new widget card.
    wx0, wy0 = 1150, 460
    for y in range(wy0, h):
        xa, xb = 1060, wx0 - 1
        ca, cb = px[xa, y], px[xb, y]
        slope = [(cb[i] - ca[i]) / (xb - xa) for i in range(3)]
        for x in range(wx0, w):
            t = x - xb
            rgb = tuple(clamp(cb[i] + slope[i] * t * 0.55) for i in range(3))
            a = min(1.0, (x - wx0) / 70.0) * min(1.0, (y - wy0) / 90.0)
            base = px[x, y]
            px[x, y] = mix(base, rgb, a)

    # --- bottom strip (pill, dock) -----------------------------------------
    by0 = 660
    src0, src1 = 560, 660
    src = img.crop((0, src0, w, src1))
    src = src.filter(ImageFilter.GaussianBlur(3))
    src = src.resize((w, h - by0), Image.LANCZOS)
    mask = Image.new("L", (w, h - by0), 255)
    mp = mask.load()
    feather = min(30, (h - by0) // 2)
    for y in range(h - by0):
        a = int(255 * min(1.0, y / max(1, feather)))
        for x in range(w):
            mp[x, y] = a
    img.paste(src, (0, by0), mask)

    return img


def make_wallpaper() -> Image.Image:
    """Procedural take on the reference wallpaper.

    The bands, colours and glow positions are sampled from the supplied design
    reference, then re-rendered as a clean, high-resolution gradient field.
    This keeps the look of the reference without its screenshot artefacts.
    """
    src = Image.open(REFERENCE).convert("RGB")
    sw, sh = src.size

    # Sample the band colours at UI-free heights.
    samples = []
    for i in range(128):
        x = round(i / 127 * (sw - 1))
        acc = [0, 0, 0]
        n = 0
        for y in range(180, 560, 12):
            r, g, b = src.getpixel((x, y))
            acc[0] += r; acc[1] += g; acc[2] += b
            n += 1
        samples.append(tuple(v / n for v in acc))

    W, H = 640, 512
    img = Image.new("RGB", (W, H))
    px = img.load()
    tau = math.tau
    for y in range(H):
        t = y / H
        phase1 = tau * (1.05 * t) + 0.7
        phase2 = tau * (2.55 * t) + 2.1
        phase3 = tau * (0.55 * t) + 4.0
        cx_teal = W * 0.585 + 34 * math.sin(tau * 0.7 * t + 1.1)
        cx_mag = W * 0.16 + 26 * math.sin(tau * 0.9 * t + 3.4)
        cx_dark = W * 0.75 + 50 * math.sin(tau * 0.8 * t + 0.3)
        for x in range(W):
            u = x + 24 * math.sin(phase1) + 9 * math.sin(phase2) + 7 * math.sin(phase3)
            u = max(0.0, min(1.0, u / (W - 1)))
            f = u * (len(samples) - 1)
            i0 = int(f)
            i1 = min(i0 + 1, len(samples) - 1)
            k = f - i0
            r = lerp(samples[i0][0], samples[i1][0], k)
            g = lerp(samples[i0][1], samples[i1][1], k)
            b = lerp(samples[i0][2], samples[i1][2], k)

            # soft teal bloom
            gd = abs(x - cx_teal) / 92.0
            if gd < 1:
                glow = (1 - gd) ** 3 * 0.30
                r = lerp(r, 65, glow)
                g = lerp(g, 232, glow)
                b = lerp(b, 226, glow)
            # magenta bloom
            gd = abs(x - cx_mag) / 70.0
            if gd < 1:
                glow = (1 - gd) ** 3 * 0.18
                r = lerp(r, 150, glow)
                g = lerp(g, 36, glow)
                b = lerp(b, 122, glow)
            # deep shadow ribbon
            gd = abs(x - cx_dark) / 120.0
            if gd < 1:
                dark = (1 - gd) ** 2 * 0.22
                r = lerp(r, 5, dark)
                g = lerp(g, 14, dark)
                b = lerp(b, 34, dark)

            # vertical shading gives the ribbons some volume
            shade = 1.0 + 0.035 * math.sin(tau * 1.7 * t + x / W * 2.0)
            nx = (x - W / 2) / (W / 2)
            ny = (y - H / 2) / (H / 2)
            vig = 1.0 - 0.10 * (nx * nx + ny * ny)
            grain = ((x * 1103515245 + y * 12345) >> 16 & 3) - 1.5
            px[x, y] = (clamp(r * shade * vig + grain),
                        clamp(g * shade * vig + grain),
                        clamp(b * shade * vig + grain))

    img = img.filter(ImageFilter.GaussianBlur(0.4))
    return img.resize((1280, 1024), Image.LANCZOS)


def make_boot_bg(wall: Image.Image) -> Image.Image:
    small = wall.resize((160, 128), Image.LANCZOS)
    small = small.filter(ImageFilter.GaussianBlur(14))
    px = small.load()
    w, h = small.size
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            nx, ny = x / w - 0.5, y / h - 0.5
            d = min(1.0, math.hypot(nx, ny) * 1.8)
            glow = (1 - d) ** 2
            # darken heavily, then add a soft blue/teal centre glow
            r = r * 0.16 + 18 * glow
            g = g * 0.16 + 34 * glow
            b = b * 0.20 + 62 * glow
            a = 1 - 0.25 * (d ** 2)
            px[x, y] = (clamp(r * a), clamp(g * a), clamp(b * a))
    return small


# ---------------------------------------------------------------------------
# Icon helpers
# ---------------------------------------------------------------------------
S = 4  # supersampling factor


def icon_canvas(final_size):
    return Image.new("RGBA", (final_size * S, final_size * S), (0, 0, 0, 0))


def finish_icon(img, final_size, shadow=False):
    small = img.resize((final_size, final_size), Image.LANCZOS)
    if shadow:
        dark = Image.new("RGBA", small.size, (4, 10, 26, 0))
        dark.putalpha(small.split()[3].point(lambda a: int(a * 0.28)))
        dark = dark.filter(ImageFilter.GaussianBlur(final_size * 0.045))
        base = Image.new("RGBA", small.size, (0, 0, 0, 0))
        base.alpha_composite(dark, (0, max(1, int(final_size * 0.035))))
        base.alpha_composite(small)
        small = base
    return small


def sc(*vals):
    return tuple(int(round(v * S)) for v in vals)


def draw_icon_planet(size=48):
    c = icon_canvas(size)
    n = size * S
    cx, cy = 24 * S, 27 * S
    # ring behind
    ring = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    rd = ImageDraw.Draw(ring)
    rd.ellipse(sc(1, 19, 47, 37), outline=(150, 240, 255, 255), width=2 * S)
    rd.ellipse(sc(3, 21, 45, 35), outline=(60, 160, 255, 140), width=1 * S)
    grad = linear_gradient((n, n), (61, 226, 255), (150, 110, 255), (0, 0), (n, n))
    mask = ring.split()[3]
    ring = Image.composite(grad, Image.new("RGBA", (n, n), (0, 0, 0, 0)), mask)
    ring = ring.rotate(-18, resample=Image.BICUBIC, center=(cx, cy))
    c.alpha_composite(ring)

    # planet body
    sphere = radial_gradient((n, n), (215, 250, 255), (31, 72, 190),
                             center=(cx - 8 * S, cy - 10 * S), radius=32 * S)
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).ellipse(sc(7, 10, 41, 44), fill=255)
    body = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    paste_masked(body, sphere, mask)
    # terminator
    term = radial_gradient((n, n), (8, 18, 60, 0), (3, 8, 28, 150),
                           center=(cx + 14 * S, cy + 15 * S), radius=30 * S)
    term_mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(term_mask).ellipse(sc(7, 10, 41, 44), fill=255)
    term_mask = ImageChops.multiply(term_mask, Image.new("L", (n, n), 160))
    paste_masked(body, term, term_mask)
    c.alpha_composite(body)

    # front half of ring
    front_mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(front_mask).rectangle(sc(0, 27, 48, 48), fill=255)
    front = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    rmask = ImageChops.multiply(ring.split()[3], front_mask)
    paste_masked(front, grad, rmask)
    c.alpha_composite(front)

    # highlight
    hl = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    ImageDraw.Draw(hl).ellipse(sc(13, 15, 21, 20), fill=(255, 255, 255, 150))
    hl = hl.filter(ImageFilter.GaussianBlur(1.2 * S))
    c.alpha_composite(hl)
    return finish_icon(c, size, shadow=True)


def draw_icon_folder(size=48):
    c = icon_canvas(size)
    n = size * S
    # back tab
    tab = rounded_mask((n, n), sc(5, 12, 30, 26), 3 * S)
    tg = linear_gradient((n, n), (79, 135, 255), (31, 65, 170), (0, 0), (n, n))
    paste_masked(c, tg, tab)
    # back body
    bm = rounded_mask((n, n), sc(5, 17, 43, 41), 6 * S)
    bg = linear_gradient((n, n), (91, 150, 255), (25, 58, 165), (0, 0), (n, n))
    paste_masked(c, bg, bm)
    # front panel
    fm = rounded_mask((n, n), sc(10, 22, 41, 41), 5 * S)
    fg = linear_gradient((n, n), (91, 249, 218), (21, 148, 170), (0, 0), (n, n))
    paste_masked(c, fg, fm)
    # top gloss
    gl = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    ImageDraw.Draw(gl).rounded_rectangle(sc(12, 24, 38, 29), radius=2 * S,
                                         fill=(255, 255, 255, 70))
    c.alpha_composite(gl)
    # small fold line
    ImageDraw.Draw(c).line(sc(11, 31, 39, 31), fill=(11, 60, 90, 55), width=1 * S)
    return finish_icon(c, size, shadow=True)


def draw_icon_chat(size=48):
    c = icon_canvas(size)
    n = size * S
    gm = rounded_mask((n, n), sc(5, 6, 43, 38), 13 * S)
    grad = linear_gradient((n, n), (160, 118, 255), (52, 122, 255), (0, 0), (n, n))
    paste_masked(c, grad, gm)
    d = ImageDraw.Draw(c)
    d.polygon([sc(12, 33)[0:2], sc(9, 45)[0:2], sc(23, 37)[0:2]],
              fill=(95, 92, 245, 255))
    # subtle inner highlight
    hi = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    ImageDraw.Draw(hi).arc(sc(7, 8, 41, 34), 195, 330, fill=(255, 255, 255, 90),
                           width=1 * S)
    c.alpha_composite(hi)
    d.rounded_rectangle(sc(13, 15, 34, 19), radius=2 * S, fill=(255, 255, 255, 235))
    d.rounded_rectangle(sc(13, 23, 29, 27), radius=2 * S, fill=(255, 255, 255, 215))
    return finish_icon(c, size, shadow=True)


def draw_icon_settings(size=48):
    c = icon_canvas(size)
    n = size * S
    cx = cy = 24 * S
    d = ImageDraw.Draw(c)
    # gear outline
    pts = []
    teeth = 8
    for i in range(teeth * 4):
        a = (i / (teeth * 4)) * math.tau - math.pi / 2
        if i % 4 in (1, 2):
            r = 21.5 * S
        else:
            r = 17.0 * S
        pts.append((cx + math.cos(a) * r, cy + math.sin(a) * r))
    d.polygon(pts, fill=(72, 88, 124, 255))
    inner = []
    for i in range(teeth * 4):
        a = (i / (teeth * 4)) * math.tau - math.pi / 2
        r = (20.5 if i % 4 in (1, 2) else 16.2) * S
        inner.append((cx + math.cos(a) * r, cy + math.sin(a) * r))
    grad = linear_gradient((n, n), (230, 237, 255), (105, 121, 156), (0, 0), (n, n))
    gm = Image.new("L", (n, n), 0)
    ImageDraw.Draw(gm).polygon(inner, fill=255)
    paste_masked(c, grad, gm)
    # centre
    d.ellipse(sc(14, 14, 34, 34), fill=(17, 26, 49, 255))
    d.ellipse(sc(16, 16, 32, 32), fill=(40, 67, 120, 255))
    d.ellipse(sc(19, 19, 29, 29), fill=(18, 92, 180, 255))
    d.ellipse(sc(20, 20, 25, 25), fill=(130, 230, 255, 255))
    # rim light
    d.arc(sc(5, 5, 43, 43), 205, 330, fill=(255, 255, 255, 80), width=1 * S)
    return finish_icon(c, size, shadow=True)


def draw_icon_ai(size=48):
    c = icon_canvas(size)
    n = size * S
    d = ImageDraw.Draw(c)
    # faint glow
    glow = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse(sc(8, 8, 40, 40), fill=(120, 90, 255, 90))
    c.alpha_composite(glow.filter(ImageFilter.GaussianBlur(5 * S)))

    def bar(box, c0, c1, angle):
        layer = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        bm = rounded_mask((n, n), sc(*box), 5 * S)
        gg = linear_gradient((n, n), c0, c1, (0, 0), (n, n))
        paste_masked(layer, gg, bm)
        return layer.rotate(angle, resample=Image.BICUBIC, center=(24 * S, 24 * S))

    c.alpha_composite(bar((19, 5, 29, 43), (70, 244, 212), (18, 178, 160), 0))
    c.alpha_composite(bar((5, 19, 43, 29), (164, 112, 255), (98, 72, 230), 0))
    c.alpha_composite(bar((19, 5, 29, 43), (255, 110, 215), (230, 58, 170), 55))
    c.alpha_composite(bar((19, 5, 29, 43), (108, 245, 148), (45, 200, 120), -55))
    d = ImageDraw.Draw(c)
    d.ellipse(sc(21, 21, 27, 27), fill=(255, 255, 255, 235))
    return finish_icon(c, size, shadow=True)


def draw_icon_install(size=48):
    """Disk drive with a download badge - the installer shortcut."""
    c = icon_canvas(size)
    n = size * S
    d = ImageDraw.Draw(c)

    # drive body
    body = rounded_mask((n, n), sc(6, 9, 42, 33), 6 * S)
    bg = linear_gradient((n, n), (226, 235, 250), (120, 138, 172), (0, 0), (n, n))
    paste_masked(c, bg, body)
    inner = rounded_mask((n, n), sc(9, 12, 39, 27), 4 * S)
    ig = linear_gradient((n, n), (40, 58, 96), (18, 27, 49), (0, 0), (n, n))
    paste_masked(c, ig, inner)

    # slot + led
    d.rounded_rectangle(sc(12, 29, 36, 31), radius=1 * S, fill=(210, 224, 245, 200))
    d.ellipse(sc(32, 27, 35, 30), fill=(70, 240, 200, 255))

    # download badge
    badge = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    bm = Image.new("L", (n, n), 0)
    ImageDraw.Draw(bm).ellipse(sc(25, 25, 45, 45), fill=255)
    bgrad = linear_gradient((n, n), (86, 231, 209), (37, 110, 235), (0, 0), (n, n))
    paste_masked(badge, bgrad, bm)
    c.alpha_composite(badge)

    # arrow
    d = ImageDraw.Draw(c)
    d.line(sc(35, 30, 35, 38), fill=(255, 255, 255, 255), width=int(2.2 * S))
    d.polygon([sc(31.5, 36)[0:2], sc(38.5, 36)[0:2], sc(35, 41)[0:2]],
              fill=(255, 255, 255, 255))
    return finish_icon(c, size, shadow=True)


def draw_top_icon(kind, size=22):
    ss = 8
    n = size * ss
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    ink = (17, 26, 51, 255)
    blue = (62, 118, 255, 255)
    teal = (24, 196, 200, 255)
    purple = (140, 92, 255, 255)
    lw = max(1, int(1.7 * ss))

    def line(a, b, color=ink, w=lw):
        d.line([a[0] * ss, a[1] * ss, b[0] * ss, b[1] * ss], fill=color, width=w)
        r = w / 2
        for p in (a, b):
            d.ellipse([p[0] * ss - r, p[1] * ss - r, p[0] * ss + r, p[1] * ss + r], fill=color)

    if kind == "search":
        d.ellipse([4 * ss, 4 * ss, 15 * ss, 15 * ss], outline=ink, width=lw)
        line((13.4, 13.4), (18.3, 18.3), blue, lw)
    elif kind == "switch":
        d.rounded_rectangle([3 * ss, 5 * ss, 15 * ss, 16 * ss], radius=3 * ss,
                            outline=ink, width=lw)
        d.rounded_rectangle([7 * ss, 9 * ss, 19 * ss, 19 * ss], radius=3 * ss,
                            outline=blue, width=lw)
        line((10, 12.5), (15, 12.5), teal, max(1, lw - ss // 2))
    elif kind == "folder":
        d.rounded_rectangle([3 * ss, 6 * ss, 19 * ss, 18 * ss], radius=3 * ss,
                            outline=ink, width=lw)
        line((3.5, 8.5), (9, 8.5), teal)
        line((15, 10), (18, 10), teal)
    elif kind == "bell":
        d.arc([5 * ss, 3 * ss, 17 * ss, 19 * ss], 180, 360, fill=ink, width=lw)
        line((5, 11), (5, 15), ink)
        line((17, 11), (17, 15), ink)
        line((3.5, 16), (18.5, 16), ink)
        d.ellipse([9.5 * ss, 18 * ss, 12.5 * ss, 21 * ss], fill=purple)
        d.ellipse([8.5 * ss, 4 * ss, 10 * ss, 5.5 * ss], fill=blue)
    elif kind == "wifi":
        lw = max(2, int(1.9 * ss))
        d.arc([1 * ss, 2 * ss, 19 * ss, 20 * ss], 215, 325, fill=ink, width=lw)
        d.arc([4 * ss, 5.5 * ss, 16 * ss, 17.5 * ss], 215, 325, fill=ink, width=lw)
        d.arc([7 * ss, 9 * ss, 13 * ss, 15 * ss], 215, 325, fill=ink, width=lw)
        d.ellipse([8.5 * ss, 14 * ss, 11.5 * ss, 17 * ss], fill=ink)
    elif kind == "bars":
        bars = [(2, 13, 5, 19), (7, 10, 10, 19), (12, 7, 15, 19), (17, 4, 20, 19)]
        for i, (x0, y0, x1, y1) in enumerate(bars):
            col = blue if i == 3 else ink
            d.rounded_rectangle([x0 * ss, y0 * ss, x1 * ss, y1 * ss],
                                radius=1.5 * ss, fill=col)
    out = img.resize((size, size), Image.LANCZOS)
    return out


def draw_logo(size):
    ss = 4
    n = size * ss
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    # render an R at ~82% of the canvas
    font = ImageFont.truetype(str(FONT_DIR / "Poppins-ExtraBold.ttf"), int(size * 0.9 * ss))
    mask = Image.new("L", (n, n), 0)
    md = ImageDraw.Draw(mask)
    bbox = font.getbbox("R")
    gw, gh = bbox[2] - bbox[0], bbox[3] - bbox[1]
    ox = (n - gw) // 2 - bbox[0]
    oy = (n - gh) // 2 - bbox[1]
    md.text((ox, oy), "R", font=font, fill=255)
    grad = linear_gradient((n, n), (56, 232, 255), (152, 79, 255), (0, 0), (n, n))
    grad = Image.composite(grad, Image.new("RGBA", (n, n), (0, 0, 0, 0)),
                           mask.filter(ImageFilter.GaussianBlur(0.4 * ss)))
    # outline
    outline = mask.filter(ImageFilter.MaxFilter(7)).point(lambda v: 255 if v > 70 else 0)
    shadow = Image.new("RGBA", (n, n), (3, 8, 24, 0))
    shadow.putalpha(outline.point(lambda v: int(v * 0.55)))
    shadow = shadow.filter(ImageFilter.GaussianBlur(2 * ss))
    img.alpha_composite(shadow, (0, 2 * ss))
    img.paste((9, 16, 38, 255), (0, 0), outline)
    img.alpha_composite(grad)
    # glossy top highlight on the R
    hi = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hi)
    hd.ellipse([n * 0.34, n * 0.23, n * 0.56, n * 0.37], fill=(255, 255, 255, 55))
    hi.putalpha(ImageChops.multiply(hi.split()[3], mask))
    img.alpha_composite(hi)
    return img.resize((size, size), Image.LANCZOS)


def draw_wordmark():
    text = "ReverticalOS"
    font = ImageFont.truetype(str(FONT_DIR / "Poppins-SemiBold.ttf"), 34)
    spacing = 0.6
    widths = [font.getlength(ch) for ch in text]
    total = sum(widths) + spacing * (len(text) - 1)
    w = int(math.ceil(total)) + 24
    h = 60
    mask = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(mask)
    x = 12.0
    for ch, cw in zip(text, widths):
        d.text((x, 42), ch, font=font, fill=255, anchor="ls")
        x += cw + spacing
    grad = linear_gradient((w, h), (255, 255, 255), (178, 220, 255), (0, 10), (0, h))
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)
    return img


def draw_cursor():
    ss = 6
    w, h = 22, 32
    img = Image.new("RGBA", (w * ss, h * ss), (0, 0, 0, 0))
    pts = [(2, 1.5), (2, 24), (7.2, 19.2), (12, 29.5), (16.2, 27.6), (11.6, 17.4), (19.5, 17.4)]
    pts = [(x * ss, y * ss) for x, y in pts]
    d = ImageDraw.Draw(img)
    # shadow
    sh = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).polygon([(x + 1.5 * ss, y + 2 * ss) for x, y in pts],
                               fill=(3, 8, 20, 110))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(1.2 * ss)))
    d.polygon(pts, fill=(255, 255, 255, 255), outline=(9, 16, 38, 255))
    d.line(pts + [pts[0]], fill=(9, 16, 38, 255), width=int(1.6 * ss), joint="curve")
    return img.resize((w, h), Image.LANCZOS)


# ---------------------------------------------------------------------------
# Font atlases
# ---------------------------------------------------------------------------
CHARSET = "".join(chr(c) for c in range(32, 127)) + "°"


def build_font(name, font_file, px):
    font = ImageFont.truetype(str(FONT_DIR / font_file), px)
    ascent, descent = font.getmetrics()
    glyphs = []
    padded = []
    x = 2
    y = 2
    row_h = 0
    atlas_w = 512
    for ch in CHARSET:
        adv = int(round(font.getlength(ch)))
        cw = max(adv + 24, 32)
        chh = ascent + descent + 24
        tmp = Image.new("L", (cw, chh), 0)
        ImageDraw.Draw(tmp).text((12, 12 + ascent), ch, font=font,
                                 fill=255, anchor="ls")
        bbox = tmp.getbbox()
        if bbox:
            ox = bbox[0] - 12
            oy_rel = bbox[1] - (12 + ascent)
            gw, gh = bbox[2] - bbox[0], bbox[3] - bbox[1]
            mask = tmp.crop(bbox)
            if x + gw + 2 > atlas_w:
                x = 2
                y += row_h + 2
                row_h = 0
            padded.append((ch, x, y, gw, gh, ox, oy_rel, adv, mask))
            x += gw + 2
            row_h = max(row_h, gh)
        else:
            padded.append((ch, 0, 0, 0, 0, 0, 0, adv, None))
    atlas_h = y + row_h + 4
    atlas = Image.new("RGBA", (atlas_w, atlas_h), (0, 0, 0, 255))
    full = Image.new("L", (atlas_w, atlas_h), 255)
    atlas = atlas.convert("RGB").convert("RGBA")
    for ch, gx, gy, gw, gh, ox, oy_rel, adv, mask in padded:
        if mask is not None:
            cover = mask.convert("L")
            glyph = Image.merge("RGBA", (cover, Image.new("L", cover.size, 0),
                                         Image.new("L", cover.size, 0),
                                         Image.new("L", cover.size, 255)))
            atlas.paste(glyph, (gx, gy))
        glyphs.append((ord(ch), gx, gy, gw, gh, ox, oy_rel, adv))
    atlas = Image.composite(atlas, Image.new("RGBA", (atlas_w, atlas_h), (0, 0, 0, 255)), full)
    return {
        "name": name,
        "atlas": atlas,
        "ascent": ascent,
        "line_height": ascent + descent,
        "glyphs": glyphs,
    }


# ---------------------------------------------------------------------------
# Packing
# ---------------------------------------------------------------------------
ASSETS = [
    ("wallpaper", None),
    ("bootbg", None),
    ("logo_lg", None),
    ("logo_sm", None),
    ("wordmark", None),
    ("dock_planet", None),
    ("dock_folder", None),
    ("dock_chat", None),
    ("dock_settings", None),
    ("dock_ai", None),
    ("dock_install", None),
    ("icon_search", None),
    ("icon_switch", None),
    ("icon_folder", None),
    ("icon_bell", None),
    ("wifi", None),
    ("bars", None),
    ("cursor", None),
    ("font_ui13", None),
    ("font_ui15", None),
    ("font_ui15sb", None),
    ("font_title19", None),
]


def main():
    BUILD.mkdir(exist_ok=True)
    PREVIEW.mkdir(exist_ok=True)

    print("wallpaper...")
    wallpaper = make_wallpaper()
    wallpaper.save(PREVIEW / "wallpaper.png")
    wall_small = wallpaper.resize((320, 256), Image.LANCZOS)
    wall_small.save(PREVIEW / "wallpaper_640.png")

    print("boot background...")
    bootbg = make_boot_bg(wall_small)
    bootbg.resize((640, 512), Image.LANCZOS).save(PREVIEW / "bootbg.png")

    print("icons...")
    icons = {
        "dock_planet": draw_icon_planet(48),
        "dock_folder": draw_icon_folder(48),
        "dock_chat": draw_icon_chat(48),
        "dock_settings": draw_icon_settings(48),
        "dock_ai": draw_icon_ai(48),
        "dock_install": draw_icon_install(48),
        "icon_search": draw_top_icon("search"),
        "icon_switch": draw_top_icon("switch"),
        "icon_folder": draw_top_icon("folder"),
        "icon_bell": draw_top_icon("bell"),
        "wifi": draw_top_icon("wifi", 20),
        "bars": draw_top_icon("bars", 18),
        "cursor": draw_cursor(),
        "logo_lg": draw_logo(160),
        "logo_sm": draw_logo(28),
        "wordmark": draw_wordmark(),
    }

    # contact sheet
    sheet = Image.new("RGBA", (640, 300), (20, 28, 52, 255))
    x = 16
    for key in ("dock_planet", "dock_folder", "dock_chat", "dock_settings",
                "dock_ai", "dock_install"):
        sheet.alpha_composite(icons[key], (x, 20))
        x += 70
    x = 20
    for key in ("icon_search", "icon_switch", "icon_folder", "icon_bell", "wifi", "bars"):
        sheet.alpha_composite(icons[key], (x, 110))
        x += 40
    sheet.alpha_composite(icons["logo_lg"], (20, 150))
    sheet.alpha_composite(icons["logo_sm"], (170, 170))
    sheet.alpha_composite(icons["wordmark"], (20, 245))
    sheet.alpha_composite(icons["cursor"], (230, 160))
    sheet.save(PREVIEW / "icons.png")

    print("fonts...")
    fonts = {}
    for name, file, px in [
        ("font_ui13", "Poppins-Regular.ttf", 13),
        ("font_ui15", "Poppins-Regular.ttf", 15),
        ("font_ui15sb", "Poppins-SemiBold.ttf", 15),
        ("font_title19", "Poppins-SemiBold.ttf", 19),
    ]:
        fonts[name] = build_font(name, file, px)
        fonts[name]["atlas"].save(PREVIEW / (name + ".png"))

    blobs = {}
    blobs["wallpaper"] = qoi_encode(wall_small)
    blobs["bootbg"] = qoi_encode(bootbg)
    for key, img in icons.items():
        blobs[key] = qoi_encode(img)
    for key, f in fonts.items():
        blobs[key] = qoi_encode(f["atlas"])

    # sanity: round-trip one image
    test = qoi_decode(blobs["dock_planet"])
    assert test.size == icons["dock_planet"].size
    assert test.tobytes() == icons["dock_planet"].tobytes(), "qoi roundtrip failed"

    total = sum(len(v) for v in blobs.values())
    print(f"asset payload: {total} bytes")
    for k, v in sorted(blobs.items(), key=lambda kv: -len(kv[1])):
        print(f"  {k:14s} {len(v):7d}")

    # assemble container
    ids = {name: i + 1 for i, (name, _) in enumerate(ASSETS)}
    for name in blobs:
        assert name in ids, name
    header = bytearray(b"RVAS" + struct.pack("<I", len(ASSETS)))
    entry_size = 12
    data_start = len(header) + entry_size * len(ASSETS)
    data_start = (data_start + 15) & ~15
    entries = bytearray()
    payload = bytearray()
    for name, _ in ASSETS:
        blob = blobs[name]
        entries += struct.pack("<III", ids[name], data_start + len(payload), len(blob))
        payload += blob
        while len(payload) % 4:
            payload += b"\0"
    out = bytearray()
    out += b"RVAS" + struct.pack("<I", len(ASSETS))
    out += entries
    out += b"\0" * (data_start - len(out))
    out += payload
    BUILD.mkdir(exist_ok=True)
    (BUILD / "assets.bin").write_bytes(out)
    print(f"assets.bin: {len(out)} bytes")

    # generated C
    idlines = ["/* Generated by tools/genassets.py - do not edit. */",
               "#ifndef RVOS_ASSETS_IDS_H", "#define RVOS_ASSETS_IDS_H", ""]
    for name, _ in ASSETS:
        idlines.append(f"#define ASSET_{name.upper()} {ids[name]}u")
    idlines.append("")
    idlines.append("#endif")
    (ROOT / "kernel" / "assets_ids.h").write_text("\n".join(idlines))

    lines = []
    lines.append("/* Generated by tools/genassets.py - do not edit. */")
    lines.append("#ifndef RVOS_ASSETS_DATA_H")
    lines.append("#define RVOS_ASSETS_DATA_H")
    lines.append("")
    lines.append("#include \"types.h\"")
    lines.append("#include \"assets.h\"")
    lines.append("")
    for name, f in fonts.items():
        lines.append("static const GlyphMetric g_%s_glyphs[] = {" % name)
        for cp, x, y, w, h, ox, oy, adv in f["glyphs"]:
            lines.append("    {%d,%d,%d,%d,%d,%d,%d,%d}," % (cp, x, y, w, h, ox, oy, adv))
        lines.append("};")
        lines.append("const FontDef g_%s = { ASSET_%s, %du, %du, %du, g_%s_glyphs };" %
                     (name, name.upper(), f["ascent"], f["line_height"],
                      len(f["glyphs"]), name))
        lines.append("")
    lines.append("#endif")
    (ROOT / "kernel" / "assets_data.c").write_text("\n".join(lines))

    print("done")


if __name__ == "__main__":
    main()
