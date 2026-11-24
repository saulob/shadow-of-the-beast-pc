"""The baked 1920x1080 DXT1 controls page shown before Press Start, redrawn for Xbox.

Only the controller, the caption lines and the icons set into the text are repainted; every block that does not change keeps
its original bytes.
"""
from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

import sotb_texture as tex
import xbox_assets as assets
import xbox_glyphs as glyphs
from sotb_pak import Pak

ROOT = Path(__file__).resolve().parents[2]
LINE_ART = ROOT / "assets" / "source" / "controller" / "xbox-series-lineart.png"
NAME = assets.CONTENT + "HUD/PressStartScreen/ControllerMapStart"

WIDTH, HEIGHT = 1920, 1080
SS = 4
RED = (197, 0, 0)
LINE_WIDTH = 3.0
DOT = 5.0
ART_BOX = (11, 11, 1012, 681)
ART_CENTER = (958.0, 580.0)
SCALE = 0.58
STROKE_BOOST = 1.9

CONTROLLER_ZONE = (620, 395, 1300, 770)
LINE_ZONE = (440, 130, 1425, 840)
KEEP_ZONES = ((930, 140, 1190, 215), (0, 800, 520, 1080))
ICON_ZONES = {
    "counter_lb": (448, 252, 520, 292), "rage_lb": (786, 160, 851, 196), "rage_rb": (872, 160, 938, 196),
    "special_rt": (1362, 82, 1431, 120), "block_rb": (1362, 248, 1431, 292),
    "or_y": (1578, 118, 1620, 158), "or_x": (1646, 118, 1688, 158), "or_b": (1714, 118, 1758, 158),
    "attack_y": (1380, 405, 1428, 460), "stun_x": (1380, 462, 1428, 517), "grab_b": (1380, 520, 1428, 575), "jump_a": (1380, 588, 1428, 642),
    "using_x": (374, 509, 406, 543), "using_b": (411, 509, 445, 543),
}
ICON_GLYPH = {
    "counter_lb": "LB", "rage_lb": "LB", "rage_rb": "RB", "special_rt": "RT", "block_rb": "RB",
    "or_y": "Y", "or_x": "X", "or_b": "B", "attack_y": "Y", "stun_x": "X", "grab_b": "B", "jump_a": "A", "using_x": "X", "using_b": "B",
}
DISC_LETTER = {"A": (52, 142, 42), "B": (196, 48, 48), "X": (40, 104, 196), "Y": (186, 134, 12)}


def art_point(ax: float, ay: float) -> tuple[float, float]:
    cx = (ART_BOX[0] + ART_BOX[2]) / 2
    cy = (ART_BOX[1] + ART_BOX[3]) / 2
    return ART_CENTER[0] + (ax - cx) * SCALE, ART_CENTER[1] + (ay - cy) * SCALE


def routes() -> list[tuple[list[tuple[float, float]], tuple[float, float]]]:
    """Caption lines, each from the caption to the Xbox control that the PlayStation button is bound to."""
    p = art_point
    lb, rb, rt = p(238, 62), p(800, 62), p(800, 14)
    y, x, b, a = p(771, 155), p(705, 222), p(840, 222), p(771, 288)
    view, left_stick, right_stick, dpad = p(448, 214), p(243, 208), p(645, 370), p(318, 372)
    lines = []

    def add(points):
        lines.append((points, points[-1]))

    add([(1421, 141), rt])
    add([(461, 293), lb])
    add([(1421, 302), rb])
    add([(962, 328), (962, 372), (905, 372), (lb[0] + 12, lb[1] - 11)])
    add([(962, 372), (1020, 372), (rb[0] - 12, rb[1] - 11)])
    add([(1389, 442), (1190, 442), (y[0] + 17, y[1] - 12)])
    add([(1386, 497), (1320, 497), (1320, 424), (x[0] - 6, 424), (x[0] - 6, x[1] - 17)])
    add([(1383, 555), (1200, 555), (b[0] + 15, b[1] + 15)])
    add([(1387, 625), (1160, 625), (a[0] + 14, a[1] + 14)])
    add([(1390, 830), (1287, 830), (right_stick[0] + 6, right_stick[1] + 14)])
    add([(461, 478), (740, 478), left_stick])
    add([(535, 478), (650, 595), dpad])
    add([(961, 822), (961, 700), (930, 700), (930, view[1] + 14), (view[0], view[1] + 14)])
    return lines


def _luma(image: np.ndarray) -> np.ndarray:
    return image[..., 0] * 0.299 + image[..., 1] * 0.587 + image[..., 2] * 0.114


def _dilate(mask: np.ndarray, radius: int) -> np.ndarray:
    image = Image.fromarray((mask * 255).astype(np.uint8))
    return np.asarray(image.filter(ImageFilter.MaxFilter(radius * 2 + 1))) > 0


def _in_box(shape, box) -> np.ndarray:
    out = np.zeros(shape, dtype=bool)
    out[box[1]:box[3], box[0]:box[2]] = True
    return out


def _gaussian(values: np.ndarray, sigma: float) -> np.ndarray:
    radius = int(sigma * 3)
    kernel = np.exp(-0.5 * (np.arange(-radius, radius + 1) / sigma) ** 2)
    kernel /= kernel.sum()
    padded = np.pad(values, ((radius, radius), (0, 0)) + ((0, 0),) * (values.ndim - 2), mode="edge")
    out = sum(kernel[i] * padded[i:i + values.shape[0]] for i in range(len(kernel)))
    padded = np.pad(out, ((0, 0), (radius, radius)) + ((0, 0),) * (values.ndim - 2), mode="edge")
    return sum(kernel[i] * padded[:, i:i + values.shape[1]] for i in range(len(kernel)))


def _coarse_gaussian(values: np.ndarray, sigma: float, factor: int = 4) -> np.ndarray:
    """A wide Gaussian on a reduced copy, for large holes."""
    height, width = values.shape[:2]
    pad_h, pad_w = (-height) % factor, (-width) % factor
    padded = np.pad(values, ((0, pad_h), (0, pad_w)) + ((0, 0),) * (values.ndim - 2), mode="edge")
    small = padded.reshape(padded.shape[0] // factor, factor, padded.shape[1] // factor, factor, *padded.shape[2:]).mean((1, 3))
    blurred = _gaussian(small, sigma / factor)
    channels = blurred.shape[2] if blurred.ndim == 3 else 1
    layers = [blurred[..., c] if blurred.ndim == 3 else blurred for c in range(channels)]
    up = [np.asarray(Image.fromarray(layer.astype(np.float32), mode="F").resize((padded.shape[1], padded.shape[0]), Image.BILINEAR)) for layer in layers]
    out = np.stack(up, -1) if blurred.ndim == 3 else up[0]
    return out[:height, :width]


def restore_background(image: np.ndarray, mask: np.ndarray) -> np.ndarray:
    """Fills the masked pixels from the unmasked ones around them, smooth at a few scales."""
    rows, columns = np.where(mask.any(1))[0], np.where(mask.any(0))[0]
    top, bottom = max(0, rows[0] - 40), min(mask.shape[0], rows[-1] + 41)
    left, right = max(0, columns[0] - 40), min(mask.shape[1], columns[-1] + 41)
    crop, hole = image[top:bottom, left:right].astype(np.float64), mask[top:bottom, left:right]
    keep = (~hole).astype(np.float64)
    result = crop.copy()
    remaining = hole.copy()
    for sigma in (2, 6, 16, 48, 120):
        blur = _gaussian if sigma < 16 else _coarse_gaussian
        weight = blur(keep, sigma)
        value = blur(crop * keep[..., None], sigma)
        usable = remaining & (weight > 0.03)
        result[usable] = value[usable] / weight[usable][:, None]
        remaining &= ~usable
    out = image.astype(np.float64).copy()
    out[top:bottom, left:right] = result
    return out


def _removal_mask(image: np.ndarray, icons: dict[str, tuple[int, int, int, int]]) -> np.ndarray:
    luma = _luma(image.astype(np.float64))
    mask = np.zeros(luma.shape, dtype=bool)
    zone = _in_box(luma.shape, CONTROLLER_ZONE)
    mask |= zone & (luma > 66)
    red = (image[..., 0] > 90) & (image[..., 0].astype(int) - image[..., 1] > 50)
    lines = _in_box(luma.shape, LINE_ZONE)
    for keep in KEEP_ZONES:
        lines &= ~_in_box(luma.shape, keep)
    mask |= red & lines
    mask = _dilate(mask, 3)
    for left, top, right, bottom in icons.values():
        mask |= _in_box(luma.shape, (left - 3, top - 3, right + 3, bottom + 3))
    return mask


def icon_boxes(image: np.ndarray) -> dict[str, tuple[int, int, int, int]]:
    luma = _luma(image.astype(np.float64))
    boxes = {}
    for name, (left, top, right, bottom) in ICON_ZONES.items():
        bright = luma[top:bottom, left:right] > 150
        columns, rows = np.where(bright.any(0))[0], np.where(bright.any(1))[0]
        if not len(columns) or not len(rows):
            raise ValueError(f"no icon in zone {name}")
        boxes[name] = (left + int(columns[0]), top + int(rows[0]), left + int(columns[-1]) + 1, top + int(rows[-1]) + 1)
    return boxes


def _icon_image(name: str, width: int, height: int) -> np.ndarray:
    big = Image.new("RGBA", (width * SS, height * SS), (0, 0, 0, 0))
    draw = ImageDraw.Draw(big)
    text = ICON_GLYPH[name]
    if text in DISC_LETTER:
        draw.ellipse([0, 0, width * SS - 1, height * SS - 1], fill=(246, 246, 246, 255))
        font = glyphs._font(min(width, height) * SS * 0.60 / 0.729)
        stroke = max(1, int(min(width, height) * SS * 0.035))
        box = draw.textbbox((0, 0), text, font=font, stroke_width=stroke)
        draw.text((width * SS / 2 - (box[0] + box[2]) / 2, height * SS / 2 - (box[1] + box[3]) / 2), text,
                  fill=DISC_LETTER[text] + (255,), font=font, stroke_width=stroke)
    else:
        radius = height * SS * 0.22
        draw.rounded_rectangle([0, 0, width * SS - 1, height * SS - 1], radius=radius, fill=(236, 236, 236, 255), outline=(196, 196, 196, 255), width=max(2, SS))
        font = glyphs._font(height * SS * 0.56 / 0.729)
        stroke = max(1, int(height * SS * 0.03))
        box = draw.textbbox((0, 0), text, font=font, stroke_width=stroke)
        draw.text((width * SS / 2 - (box[0] + box[2]) / 2, height * SS / 2 - (box[1] + box[3]) / 2), text, fill=(40, 40, 40, 255), font=font, stroke_width=stroke)
    return glyphs._premultiplied_reduce(np.asarray(big), SS)


def _blend(base: np.ndarray, layer: np.ndarray, left: int, top: int) -> None:
    height, width = layer.shape[:2]
    alpha = layer[..., 3:4].astype(np.float64) / 255.0
    region = base[top:top + height, left:left + width]
    region[:] = np.clip(np.rint(layer[..., :3] * alpha + region * (1 - alpha)), 0, 255)


def _stroke_layer(polylines, dots) -> np.ndarray:
    image = Image.new("L", (WIDTH * SS, HEIGHT * SS), 0)
    draw = ImageDraw.Draw(image)
    for points in polylines:
        scaled = [(px * SS, py * SS) for px, py in points]
        draw.line(scaled, fill=255, width=int(LINE_WIDTH * SS), joint="curve")
        for px, py in scaled:
            r = LINE_WIDTH * SS / 2
            draw.ellipse([px - r, py - r, px + r, py + r], fill=255)
    for px, py in dots:
        r = DOT * SS
        draw.ellipse([px * SS - r, py * SS - r, px * SS + r, py * SS + r], fill=255)
    return np.asarray(image.resize((WIDTH, HEIGHT), Image.BOX), dtype=np.float64) / 255.0


def _controller_layer() -> np.ndarray:
    art = Image.open(LINE_ART).convert("RGBA").split()[3].crop(ART_BOX)
    size = (round(art.width * SCALE), round(art.height * SCALE))
    ink = np.clip(np.asarray(art.resize(size, Image.LANCZOS), dtype=np.float64) * STROKE_BOOST, 0, 255) / 255.0
    layer = np.zeros((HEIGHT, WIDTH))
    left, top = round(ART_CENTER[0] - size[0] / 2), round(ART_CENTER[1] - size[1] / 2)
    layer[top:top + size[1], left:left + size[0]] = ink
    return layer


def redraw(original: np.ndarray) -> np.ndarray:
    icons = icon_boxes(original)
    mask = _removal_mask(original, icons)
    canvas = restore_background(original, mask)
    body = _controller_layer()
    canvas = canvas * (1 - body[..., None]) + np.array([242.0, 242.0, 242.0]) * body[..., None]
    lines = routes()
    stroke = _stroke_layer([points for points, _ in lines], [dot for _, dot in lines])
    canvas = canvas * (1 - stroke[..., None]) + np.array(RED, dtype=np.float64) * stroke[..., None]
    canvas = np.clip(np.rint(canvas), 0, 255)
    for name, (left, top, right, bottom) in icons.items():
        _blend(canvas, _icon_image(name, right - left, bottom - top), left, top)
    return canvas.astype(np.uint8)


def start_map_replacement(pak: Pak) -> assets.Replacement:
    original = pak.read(pak.entry(NAME + ".uasset"))
    texture = tex.parse_texture(original)
    if (texture.pixel_format, texture.width, texture.height, len(texture.mips)) != ("PF_DXT1", WIDTH, HEIGHT, 1):
        raise ValueError("ControllerMapStart is not the expected 1920x1080 DXT1 texture")
    mip = texture.mips[0]
    before = tex.decode_mip(original, texture, 0)[..., :3]
    after = redraw(before)
    columns, rows = WIDTH // 4, HEIGHT // 4
    blocks = tex.untile(original[mip.start:mip.start + mip.size], columns, rows, 8).reshape(-1, 8).copy()
    changed = (after != before).reshape(rows, 4, columns, 4, 3).any((1, 3, 4)).reshape(-1)
    alpha = np.full(after.shape[:2] + (1,), 255, dtype=np.uint8)
    encoded = np.frombuffer(tex.encode_bc1(np.concatenate([after, alpha], -1), changed), dtype=np.uint8).reshape(-1, 8)
    blocks[changed] = encoded
    data = bytearray(original)
    data[mip.start:mip.start + mip.size] = tex.tile(blocks, columns, rows, 8)
    return assets.Replacement(NAME, original, bytes(data), texture, "Press Start controls page")
