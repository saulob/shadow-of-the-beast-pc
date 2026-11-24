"""The Xbox replacement for every PlayStation prompt texture of Shadow of the Beast, as full package images."""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from PIL import Image

import sotb_texture as tex
import xbox_glyphs as glyphs
from sotb_pak import Pak

CONTENT = "Beast/Content/"

FACE = {"Cross": "A", "Circle": "B", "Square": "X", "Triangle": "Y"}

FONT = CONTENT + "HUD/Fonts/ButtonIconFont_48"
FONT_GRID = 46
FONT_GLYPHS = [
    (0, 0, 1, 1, "face:A"),
    (1, 0, 2, 1, "face:B"),
    (2, 0, 3, 1, "menu"),
    (3, 0, 5, 1, "bumper:LB"),
    (0, 1, 1, 2, "face:X"),
    (1, 1, 2, 2, "face:Y"),
    (2, 1, 3, 2, "dpad"),
    (3, 1, 4, 2, "stick:L"),
    (4, 1, 5, 2, "stick:R"),
    (0, 2, 2, 3, "bumper:RB"),
    (3, 2, 5, 3, "view"),
    (1, 3, 2, 4, "click:L"),
    (2, 3, 3, 4, "click:R"),
    (3, 3, 4, 4, "trigger:LT"),
    (1, 4, 3, 5, "clicks"),
    (3, 4, 4, 5, "trigger:RT"),
]


def _face_mask(pixels: np.ndarray, kind: str) -> np.ndarray:
    r, g, b = pixels[..., 0], pixels[..., 1], pixels[..., 2]
    if kind == "face:A":
        return (b > r + 20) & (b > g + 10)
    if kind == "face:B":
        return (r > 150) & (g < 100) & (b < 120)
    if kind == "face:X":
        return (r > 200) & (b > 140) & (r > g + 40)
    return (g > r + 20) & (g > b + 10)


@dataclass
class Replacement:
    name: str
    original: bytes
    replaced: bytes
    texture: tex.Texture
    purpose: str


def _glyph_specs() -> list[tuple[str, str, bool, bool, str]]:
    """(asset, glyph kind, pressed, outline, purpose)."""
    out: list[tuple[str, str, bool, bool, str]] = []
    hud = CONTENT + "HUD/Textures/"
    for pressed in (False, True):
        suffix = "Pressed" if pressed else ""
        for ps, letter in FACE.items():
            out.append((f"{hud}Button{ps}{suffix}", f"face:{letter}", pressed, False, f"HUD {ps} prompt"))
        out.append((f"{hud}ButtonL1{suffix}", "bumper:LB", pressed, False, "HUD L1 prompt"))
        out.append((f"{hud}ButtonR1{suffix}", "bumper:RB", pressed, False, "HUD R1 prompt"))
        out.append((f"{hud}ButtonL2{suffix}", "trigger:LT", pressed, False, "HUD L2 prompt"))
        out.append((f"{hud}ButtonR2{suffix}", "trigger:RT", pressed, False, "HUD R2 prompt"))
        out.append((f"{hud}ButtonL3{suffix}", "click:L", pressed, False, "HUD L3 prompt"))
        out.append((f"{hud}ButtonR3{suffix}", "click:R", pressed, False, "HUD R3 prompt"))
        out.append((f"{hud}ButtonL3andR3{suffix}", "clicks", pressed, False, "HUD L3 and R3 prompt"))
        out.append((f"{hud}ButtonOptions{suffix}", "menu", pressed, False, "HUD Options prompt"))
        out.append((f"{hud}ButtonTouchpad{suffix}", "view", pressed, False, "HUD Touchpad prompt"))
    out.append((f"{hud}DPad_4Way", "dpad", False, False, "HUD D-pad prompt"))
    out.append((f"{hud}LeftAnalogue", "stick:L", False, False, "HUD left stick prompt"))
    out.append((f"{hud}RightAnalogue", "stick:R", False, False, "HUD right stick prompt"))
    alt = hud + "ButtonsALT/button"
    for ps, letter in FACE.items():
        out.append((f"{alt}{ps}_ALT", f"face:{letter}", False, True, f"HUD alternate {ps} prompt"))
    for name, kind in (("L1", "bumper:LB"), ("R1", "bumper:RB"), ("L2", "trigger:LT"), ("R2", "trigger:RT"), ("L3", "click:L"),
                       ("R3", "click:R"), ("L3andR3", "clicks"), ("Options", "menu"), ("Touchpad", "view")):
        out.append((f"{alt}{name}_ALT", kind, False, True, f"HUD alternate {name} prompt"))
    settings = CONTENT + "Frontend/ControllerSettingsScreen/Textures/Buttons/"
    for ps, letter in FACE.items():
        out.append((f"{settings}{ps}", f"face:{letter}", False, False, f"Controller Settings {ps} caption"))
    for name, kind in (("L1", "bumper:LB"), ("R1", "bumper:RB"), ("L2", "trigger:LT"), ("R2", "trigger:RT"), ("L3", "click:L"),
                       ("R3", "click:R"), ("L3R3", "clicks"), ("Touchpad", "view")):
        out.append((f"{settings}{name}", kind, False, False, f"Controller Settings {name} caption"))
    generic = CONTENT + "Frontend/Generic/Buttons/Textures/"
    for name, kind in (("Button_L1", "bumper:LB"), ("Button_R1", "bumper:RB"), ("Button_L3R3", "clicks"), ("ButtonL2", "trigger:LT"),
                       ("ButtonR2", "trigger:RT"), ("touchpad", "view"), ("L1", "bumper:LB"), ("R1", "bumper:RB")):
        out.append((f"{generic}{name}", kind, False, False, f"Menu {name} hint"))
    out.append((CONTENT + "Frontend/MapScreen/3DPanels/ManaPopup/ButtonTriangle", "face:Y", False, False, "Wisdom of Shadows Triangle prompt"))
    return out


def _premultiplied_resize(image: np.ndarray, width: int, height: int) -> np.ndarray:
    alpha = image[..., 3:4].astype(np.float64) / 255.0
    rgb = image[..., :3].astype(np.float64) * alpha
    layers = np.concatenate([rgb, alpha * 255.0], -1).astype(np.float32)
    resized = np.stack([np.asarray(Image.fromarray(layers[..., c], mode="F").resize((width, height), Image.BOX)) for c in range(4)], -1)
    a = resized[..., 3:4] / 255.0
    out = np.zeros((height, width, 4), dtype=np.uint8)
    out[..., :3] = np.clip(np.rint(resized[..., :3] / np.maximum(a, 1e-6)), 0, 255).astype(np.uint8)
    out[..., 3] = np.clip(np.rint(resized[..., 3]), 0, 255).astype(np.uint8)
    out[out[..., 3] == 0] = 0
    return out


def _average_color(image: np.ndarray, width: int, height: int) -> np.ndarray:
    alpha = image[..., 3].astype(np.float64) / 255.0
    weight = alpha.sum()
    if weight <= 0:
        color = np.zeros(3)
    else:
        color = (image[..., :3].astype(np.float64) * alpha[..., None]).sum((0, 1)) / weight
    out = np.zeros((height, width, 4), dtype=np.uint8)
    out[..., :3] = np.rint(color).astype(np.uint8)
    out[..., 3] = int(round(alpha.mean() * 255))
    return out


def write_mips(package: bytes, texture: tex.Texture, base: np.ndarray, solid_from: int | None = None) -> bytes:
    """The package with every mip rewritten from the base image. Mips of at most 4x4 texels become one solid element."""
    data = bytearray(package)
    for level, mip in enumerate(texture.mips):
        if level == 0:
            image = base
        elif mip.width <= 4 and mip.height <= 4:
            image = _average_color(base, mip.width, mip.height)
        else:
            image = _premultiplied_resize(base, mip.width, mip.height)
        solid = level > 0 and mip.width <= 4 and mip.height <= 4
        data[mip.start:mip.start + mip.size] = tex.encode_mip(texture, level, image, solid_tail=solid)
    return bytes(data)


def glyph_replacements(pak: Pak) -> list[Replacement]:
    out = []
    for name, kind, pressed, outline, purpose in _glyph_specs():
        original = pak.read(pak.entry(name + ".uasset"))
        texture = tex.parse_texture(original)
        base = tex.decode_mip(original, texture, 0)
        x0, y0, x1, y1 = glyphs.footprint(base)
        if kind == "menu":
            side = round(min(texture.width, texture.height) * 0.92)
            x0, y0 = (texture.width - side) // 2, (texture.height - side) // 2
            x1, y1 = x0 + side, y0 + side
        glyph = glyphs.render_glyph(kind, x1 - x0, y1 - y0, pressed=pressed, outline=outline)
        canvas = glyphs.fit_to_footprint(glyph, texture.width, texture.height, (x0, y0, x1, y1))
        out.append(Replacement(name, original, write_mips(original, texture, canvas), texture, purpose))
    return out


def _components(mask: np.ndarray) -> list[tuple[int, int, int, int, int, float, float]]:
    height, width = mask.shape
    seen = np.zeros_like(mask, dtype=bool)
    out = []
    for y, x in zip(*np.nonzero(mask)):
        if seen[y, x]:
            continue
        stack = [(int(y), int(x))]
        seen[y, x] = True
        x0 = x1 = int(x)
        y0 = y1 = int(y)
        count = 0
        while stack:
            cy, cx = stack.pop()
            count += 1
            x0, x1 = min(x0, cx), max(x1, cx)
            y0, y1 = min(y0, cy), max(y1, cy)
            for ny in (cy - 1, cy, cy + 1):
                for nx in (cx - 1, cx, cx + 1):
                    if 0 <= ny < height and 0 <= nx < width and mask[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        stack.append((ny, nx))
        if count >= 16:
            out.append((count, x0, y0, x1 + 1, y1 + 1, (x0 + x1 + 1) / 2, (y0 + y1 + 1) / 2))
    return out


def _font_boxes(image: np.ndarray) -> list[tuple[tuple[int, int, int, int], str]]:
    components = _components(image[..., 3] > 8)
    out = []
    for c0, r0, c1, r1, kind in FONT_GLYPHS:
        left, top, right, bottom = c0 * FONT_GRID, r0 * FONT_GRID, c1 * FONT_GRID, r1 * FONT_GRID
        chosen = [c for c in components if left <= c[5] < right and top <= c[6] < bottom]
        if not chosen:
            raise SystemExit(f"{FONT}: no PlayStation glyph in cell {c0},{r0}")
        box = (min(c[1] for c in chosen), min(c[2] for c in chosen), max(c[3] for c in chosen), max(c[4] for c in chosen))
        if kind.startswith("face:"):
            x0, y0, x1, y1 = box
            pixels = image[y0:y1, x0:x1, :3].astype(np.int32)
            if int(_face_mask(pixels, kind).sum()) < 60:
                raise SystemExit(f"{FONT}: cell {c0},{r0} is not {kind}")
        out.append((box, kind))
    return out


def _draw_font_page(image: np.ndarray, boxes) -> np.ndarray:
    height, width = image.shape[:2]
    out = image.copy()
    for (x0, y0, x1, y1), kind in boxes:
        out[max(0, y0 - 1):min(height, y1 + 1), max(0, x0 - 1):min(width, x1 + 1)] = 0
        out[y0:y1, x0:x1] = glyphs.render_glyph(kind, x1 - x0, y1 - y0)
    return out


def _write_font_page(package: bytes, texture: tex.Texture, image: np.ndarray, boxes) -> bytes:
    if len(texture.mips) != 1:
        raise SystemExit(f"{FONT}: the glyph page must have one mip")
    mip = texture.mips[0]
    block = texture.block
    columns, rows = (mip.width + block - 1) // block, (mip.height + block - 1) // block
    original = tex.untile(package[mip.start:mip.start + mip.size], columns, rows, texture.element_bytes).reshape(rows, columns, texture.element_bytes)
    encoded = np.frombuffer(tex.encode_bc3(image), dtype=np.uint8).reshape(rows, columns, texture.element_bytes)
    touched = np.zeros((rows, columns), dtype=bool)
    for (x0, y0, x1, y1), _ in boxes:
        touched[max(0, y0 - 1) // block:(y1 + 1 + block - 1) // block, max(0, x0 - 1) // block:(x1 + 1 + block - 1) // block] = True
    payload = tex.tile(np.where(touched[..., None], encoded, original).reshape(-1, texture.element_bytes), columns, rows, texture.element_bytes)
    data = bytearray(package)
    data[mip.start:mip.start + mip.size] = payload
    return bytes(data)


def font_replacement(pak: Pak) -> Replacement:
    original = pak.read(pak.entry(FONT + ".uasset"))
    pages = tex.textures(original)
    best, best_image, best_colours = None, None, 0
    for page in pages:
        image = tex.decode_mip(original, page, 0)
        pixels = image[..., :3].astype(np.int32)
        count = sum(int(_face_mask(pixels, kind).sum()) >= 30 for kind in ("face:A", "face:B", "face:X", "face:Y"))
        if count > best_colours:
            best, best_image, best_colours = page, image, count
    if best is None or best_colours < 4:
        raise SystemExit(f"{FONT}: no controller glyph page found")
    boxes = _font_boxes(best_image)
    return Replacement(FONT, original, _write_font_page(original, best, _draw_font_page(best_image, boxes), boxes), best,
                       "HUD button icon font")
