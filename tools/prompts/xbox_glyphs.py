"""Xbox button glyphs drawn from vector shapes. Every glyph is rendered for the footprint of the PlayStation glyph it replaces."""
from __future__ import annotations

import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
FONT_PATH = ROOT / "gpu" / "third_party" / "fonts" / "DejaVuSans.ttf"
SS = 4

FACE_COLORS = {
    "A": (91, 190, 74),
    "B": (217, 74, 74),
    "X": (61, 139, 217),
    "Y": (224, 185, 63),
}
WHITE = (240, 242, 246)


def _font(pixels: float) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(FONT_PATH), max(4, int(round(pixels))))


def _premultiplied_reduce(image: np.ndarray, factor: int) -> np.ndarray:
    rgb = image[..., :3].astype(np.float64) * (image[..., 3:4].astype(np.float64) / 255.0)
    alpha = image[..., 3].astype(np.float64)
    height, width = image.shape[0] // factor, image.shape[1] // factor
    rgb = rgb.reshape(height, factor, width, factor, 3).mean((1, 3))
    alpha = alpha.reshape(height, factor, width, factor).mean((1, 3))
    out = np.zeros((height, width, 4), dtype=np.uint8)
    safe = np.maximum(alpha, 1e-6)[..., None]
    out[..., :3] = np.clip(np.rint(rgb / (safe / 255.0)), 0, 255).astype(np.uint8)
    out[..., 3] = np.clip(np.rint(alpha), 0, 255).astype(np.uint8)
    out[out[..., 3] == 0] = 0
    return out


def _over(base: np.ndarray, layer: np.ndarray) -> np.ndarray:
    """Straight-alpha float RGBA arrays in 0..255."""
    a_l = layer[..., 3:4] / 255.0
    a_b = base[..., 3:4] / 255.0
    a_o = a_l + a_b * (1 - a_l)
    rgb = (layer[..., :3] * a_l + base[..., :3] * a_b * (1 - a_l)) / np.maximum(a_o, 1e-9)
    return np.concatenate([rgb, a_o * 255.0], -1)


def _solid(mask: np.ndarray, color) -> np.ndarray:
    out = np.zeros(mask.shape + (4,), dtype=np.float64)
    out[..., :3] = color
    out[..., 3] = mask * 255.0
    return out


def _mask_from(draw_fn, width: int, height: int) -> np.ndarray:
    image = Image.new("L", (width, height), 0)
    draw_fn(ImageDraw.Draw(image))
    return np.asarray(image, dtype=np.float64) / 255.0


def _text_mask(width: int, height: int, text: str, cx: float, cy: float, cap: float, weight: float = 0.05) -> np.ndarray:
    font = _font(cap / 0.729)
    stroke = max(0, int(round(cap * weight)))

    def draw(d: ImageDraw.ImageDraw) -> None:
        box = d.textbbox((0, 0), text, font=font, stroke_width=stroke)
        d.text((cx - (box[0] + box[2]) / 2, cy - (box[1] + box[3]) / 2), text, fill=255, font=font, stroke_width=stroke)

    return _mask_from(draw, width, height)


def _ellipse_mask(width: int, height: int, cx: float, cy: float, rx: float, ry: float) -> np.ndarray:
    y, x = np.mgrid[0:height, 0:width]
    return (((x + 0.5 - cx) / rx) ** 2 + ((y + 0.5 - cy) / ry) ** 2 <= 1.0).astype(np.float64)


def _smooth(values: np.ndarray, low: float, high: float) -> np.ndarray:
    t = np.clip((values - low) / (high - low), 0, 1)
    return t * t * (3 - 2 * t)


def _shade(mask: np.ndarray, top: float, bottom: float, pressed: bool, box) -> np.ndarray:
    """Dark glossy body for a shape inside `box` (x0, y0, x1, y1): vertical gradient, gloss on top, lit rim."""
    x0, y0, x1, y1 = box
    height, width = mask.shape
    y, x = np.mgrid[0:height, 0:width]
    v = np.clip((y + 0.5 - y0) / max(1.0, y1 - y0), 0, 1)
    u = np.clip((x + 0.5 - x0) / max(1.0, x1 - x0), 0, 1)
    body = top + (bottom - top) * v
    body = body * (1.0 - 0.18 * np.abs(u - 0.5) * 2)
    if pressed:
        body *= 0.72
    gloss = _smooth(0.52 - v, 0.0, 0.5) * (1 - np.abs(u - 0.5) * 1.1).clip(0, 1) * (0.07 if pressed else 0.2)
    shade = np.clip(body + 255 * gloss, 0, 255)
    rgb = np.stack([shade, shade, shade * 1.04], -1)
    return rgb


def _erode(mask: np.ndarray, radius: float) -> np.ndarray:
    size = int(radius * 2 + 1) | 1
    image = Image.fromarray((mask * 255).astype(np.uint8))
    from PIL import ImageFilter
    return np.asarray(image.filter(ImageFilter.MinFilter(size)), dtype=np.float64) / 255.0


def _dilate(mask: np.ndarray, radius: float) -> np.ndarray:
    size = int(radius * 2 + 1) | 1
    image = Image.fromarray((mask * 255).astype(np.uint8))
    from PIL import ImageFilter
    return np.asarray(image.filter(ImageFilter.MaxFilter(size)), dtype=np.float64) / 255.0


def _body(mask: np.ndarray, box, pressed: bool, rim: float = 0.07) -> np.ndarray:
    """RGBA float body with a lit rim ring of about `rim` of the shape's smaller side."""
    x0, y0, x1, y1 = box
    side = min(x1 - x0, y1 - y0)
    inner = _erode(mask, max(1.0, side * rim / 2))
    ring = np.clip(mask - inner, 0, 1)
    rgb = _shade(mask, 52, 8, pressed, box)
    rim_color = (118, 120, 128) if not pressed else (70, 72, 78)
    rgb = rgb * (1 - ring[..., None]) + np.array(rim_color) * ring[..., None]
    out = np.zeros(mask.shape + (4,), dtype=np.float64)
    out[..., :3] = rgb
    out[..., 3] = mask * 255.0
    return out


def _draw_shape(width: int, height: int, box, kind: str) -> np.ndarray:
    x0, y0, x1, y1 = box
    if kind == "ball":
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        return _ellipse_mask(width, height, cx, cy, (x1 - x0) / 2, (y1 - y0) / 2)
    if kind == "pill":
        return _mask_from(lambda d: d.rounded_rectangle([x0, y0, x1 - 1, y1 - 1], radius=(y1 - y0) * 0.4, fill=255), width, height)
    if kind == "trigger":
        w, h = x1 - x0, y1 - y0
        pts = [(x0 + w * 0.02, y1 - h * 0.03), (x0 + w * 0.10, y0 + h * 0.34)]
        for i in range(1, 24):
            a = math.pi + math.pi * i / 24
            pts.append((x0 + w * (0.5 + 0.40 * math.cos(a)), y0 + h * (0.34 + 0.34 * math.sin(a))))
        pts += [(x0 + w * 0.90, y0 + h * 0.34), (x0 + w * 0.98, y1 - h * 0.03)]
        return _mask_from(lambda d: d.polygon(pts, fill=255), width, height)
    raise ValueError(kind)


def _compose_glyph(width: int, height: int, box, kind: str, pressed: bool, marks: list[np.ndarray]) -> np.ndarray:
    mask = _draw_shape(width, height, box, kind)
    layer = _body(mask, box, pressed)
    for mark in marks:
        layer = _over(layer, mark)
    layer[..., 3] = np.minimum(layer[..., 3], mask * 255.0)
    return layer


def _letter_mark(width, height, text, box, color, pressed, scale=0.58, dy=0.0, weight=0.06):
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2 + dy * (y1 - y0)
    cap = min(x1 - x0, y1 - y0) * scale
    mask = _text_mask(width, height, text, cx, cy, cap, weight)
    shade = 0.82 if pressed else 1.0
    return _solid(mask, tuple(int(c * shade) for c in color))


def _inset(box, amount: float):
    x0, y0, x1, y1 = box
    return (x0 + amount, y0 + amount, x1 - amount, y1 - amount)


def _fit_box(width: int, height: int, aspect: float | None, inset: float):
    """The largest box of the given width/height ratio centred in the canvas, shrunk by `inset` on every side."""
    box_w, box_h = width - 2 * inset, height - 2 * inset
    if aspect is not None:
        if box_w / box_h > aspect:
            box_w = box_h * aspect
        else:
            box_h = box_w / aspect
    cx, cy = width / 2, height / 2
    return (cx - box_w / 2, cy - box_h / 2, cx + box_w / 2, cy + box_h / 2)


def _stick_marks(width, height, box, letter, pressed, click: bool):
    x0, y0, x1, y1 = box
    d = min(x1 - x0, y1 - y0)
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    ring_outer = _ellipse_mask(width, height, cx, cy, d * 0.36, d * 0.36)
    ring_inner = _ellipse_mask(width, height, cx, cy, d * 0.36 - max(1.0, d * 0.035), d * 0.36 - max(1.0, d * 0.035))
    ring = _solid(np.clip(ring_outer - ring_inner, 0, 1), (150, 152, 160))
    marks = [ring]
    if click:
        marks.append(_letter_mark(width, height, letter, (x0, y0 - d * 0.07, x1, y1 - d * 0.07), WHITE, pressed, 0.30, weight=0.07))
        ax, ay, aw = cx, cy + d * 0.18, d * 0.10
        arrow = _mask_from(lambda dr: dr.polygon([(ax - aw, ay - aw * 0.6), (ax + aw, ay - aw * 0.6), (ax, ay + aw * 0.9)], fill=255), width, height)
        marks.append(_solid(arrow, WHITE))
    else:
        marks.append(_letter_mark(width, height, letter, box, WHITE, pressed, 0.34, weight=0.07))
    return marks


def render_glyph(kind: str, width: int, height: int, pressed: bool = False, outline: bool = False) -> np.ndarray:
    """RGBA uint8 image of exactly width x height. `kind` is face:A, bumper:LB, trigger:LT, stick:L, click:L, clicks, menu, view, dpad."""
    big_w, big_h = width * SS, height * SS
    outline_px = max(2, round(min(width, height) * 0.045)) if outline else 0
    inset = outline_px * SS + SS * 0.5
    family, _, arg = kind.partition(":")
    if family == "face":
        box = _fit_box(big_w, big_h, 1.0, inset)
        marks = [_letter_mark(big_w, big_h, arg, box, FACE_COLORS[arg], pressed, 0.60)]
        layer = _compose_glyph(big_w, big_h, box, "ball", pressed, marks)
    elif family == "bumper":
        box = _fit_box(big_w, big_h, None, inset)
        x0, y0, x1, y1 = box
        plate_h = min(y1 - y0, (x1 - x0) * 0.55)
        box = (x0, (y0 + y1) / 2 - plate_h / 2, x1, (y0 + y1) / 2 + plate_h / 2)
        marks = [_letter_mark(big_w, big_h, arg, box, WHITE, pressed, 0.42, weight=0.05)]
        layer = _compose_glyph(big_w, big_h, box, "pill", pressed, marks)
    elif family == "trigger":
        box = _fit_box(big_w, big_h, None, inset)
        x0, y0, x1, y1 = box
        marks = [_letter_mark(big_w, big_h, arg, (x0, y0 + (y1 - y0) * 0.3, x1, y1), WHITE, pressed, 0.34, weight=0.07)]
        layer = _compose_glyph(big_w, big_h, box, "trigger", pressed, marks)
    elif family in ("click", "stick"):
        box = _fit_box(big_w, big_h, 1.0, inset)
        marks = _stick_marks(big_w, big_h, box, arg, pressed, family == "click")
        layer = _compose_glyph(big_w, big_h, box, "ball", pressed, marks)
    elif family == "clicks":
        d = min(big_h - 2 * inset, (big_w - 2 * inset) / 2.36)
        cy = big_h / 2
        left_box = (big_w / 2 - d * 1.18, cy - d / 2, big_w / 2 - d * 0.18, cy + d / 2)
        right_box = (big_w / 2 + d * 0.18, cy - d / 2, big_w / 2 + d * 1.18, cy + d / 2)
        layer = np.zeros((big_h, big_w, 4), dtype=np.float64)
        for box, letter in ((left_box, "L"), (right_box, "R")):
            layer = _over(layer, _compose_glyph(big_w, big_h, box, "ball", pressed, _stick_marks(big_w, big_h, box, letter, pressed, True)))
        plus_w, plus_t = d * 0.34, d * 0.07
        cx = big_w / 2
        plus = _mask_from(lambda dr: (dr.rectangle([cx - plus_w / 2, cy - plus_t / 2, cx + plus_w / 2, cy + plus_t / 2], fill=255),
                                      dr.rectangle([cx - plus_t / 2, cy - plus_w / 2, cx + plus_t / 2, cy + plus_w / 2], fill=255)), big_w, big_h)
        layer = _over(layer, _solid(plus, WHITE))
    elif family == "menu":
        box = _fit_box(big_w, big_h, 1.0, inset)
        x0, y0, x1, y1 = box
        d = min(x1 - x0, y1 - y0)
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        bars = _mask_from(lambda dr: [dr.rounded_rectangle([cx - d * 0.23, cy + k * d * 0.15 - d * 0.035, cx + d * 0.23, cy + k * d * 0.15 + d * 0.035],
                                                           radius=d * 0.035, fill=255) for k in (-1, 0, 1)], big_w, big_h)
        layer = _compose_glyph(big_w, big_h, box, "ball", pressed, [_solid(bars, tuple(int(c * (0.82 if pressed else 1)) for c in WHITE))])
    elif family == "view":
        box = _fit_box(big_w, big_h, None, inset)
        x0, y0, x1, y1 = box
        plate_h = min(y1 - y0, (x1 - x0) * 0.55)
        box = (x0, (y0 + y1) / 2 - plate_h / 2, x1, (y0 + y1) / 2 + plate_h / 2)
        cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
        s = plate_h * 0.40
        t = max(2.0, plate_h * 0.055)

        def rects(dr):
            dr.rounded_rectangle([cx - s * 0.62, cy - s * 0.70, cx + s * 0.38, cy + s * 0.18], radius=s * 0.12, outline=255, width=int(t))
            dr.rounded_rectangle([cx - s * 0.38, cy - s * 0.18, cx + s * 0.62, cy + s * 0.70], radius=s * 0.12, outline=255, width=int(t))
        icon = _mask_from(rects, big_w, big_h)
        layer = _compose_glyph(big_w, big_h, box, "pill", pressed, [_solid(icon, tuple(int(c * (0.82 if pressed else 1)) for c in WHITE))])
    elif family == "dpad":
        box = _fit_box(big_w, big_h, 1.0, inset)
        x0, y0, x1, y1 = box
        d = x1 - x0
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        arm = d * 0.36

        def plus(dr):
            dr.rounded_rectangle([x0, cy - arm / 2, x1 - 1, cy + arm / 2], radius=arm * 0.28, fill=255)
            dr.rounded_rectangle([cx - arm / 2, y0, cx + arm / 2, y1 - 1], radius=arm * 0.28, fill=255)
        mask = _mask_from(plus, big_w, big_h)
        layer = _body(mask, box, pressed, rim=0.05)
        hub = _ellipse_mask(big_w, big_h, cx, cy, d * 0.13, d * 0.13)
        hub_ring = _ellipse_mask(big_w, big_h, cx, cy, d * 0.13 - max(1.0, d * 0.02), d * 0.13 - max(1.0, d * 0.02))
        layer = _over(layer, _solid(np.clip(hub - hub_ring, 0, 1), (150, 152, 160)))
        tri = []
        for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
            px, py = cx + dx * d * 0.37, cy + dy * d * 0.37
            nx, ny = -dy, dx
            h = d * 0.065
            tri.append([(px + dx * h * 0.9, py + dy * h * 0.9), (px - dx * h * 0.7 + nx * h, py - dy * h * 0.7 + ny * h),
                        (px - dx * h * 0.7 - nx * h, py - dy * h * 0.7 - ny * h)])
        arrows = _mask_from(lambda dr: [dr.polygon(t, fill=255) for t in tri], big_w, big_h)
        layer = _over(layer, _solid(arrows, (170, 172, 180) if not pressed else (120, 122, 128)))
        layer[..., 3] = np.minimum(layer[..., 3], mask * 255.0)
    else:
        raise ValueError(kind)
    if outline:
        alpha = layer[..., 3] / 255.0
        ring = np.clip(_dilate(alpha, outline_px * SS) - alpha * 0, 0, 1)
        base = _solid(ring, (255, 255, 255))
        layer = _over(base, layer)
    return _premultiplied_reduce(np.clip(np.rint(layer), 0, 255).astype(np.uint8), SS)


def fit_to_footprint(glyph: np.ndarray, canvas_w: int, canvas_h: int, bbox) -> np.ndarray:
    """Places an image of the footprint's size at its position on a transparent canvas."""
    x0, y0, x1, y1 = bbox
    canvas = np.zeros((canvas_h, canvas_w, 4), dtype=np.uint8)
    canvas[y0:y1, x0:x1] = glyph
    return canvas


def footprint(image: np.ndarray, threshold: int = 8):
    alpha = image[..., 3] > threshold
    if not alpha.any():
        raise ValueError("empty footprint")
    cols = np.where(alpha.any(0))[0]
    rows = np.where(alpha.any(1))[0]
    return int(cols[0]), int(rows[0]), int(cols[-1]) + 1, int(rows[-1]) + 1
