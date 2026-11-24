"""Xbox controller diagram of the Controller Settings screen: the CC0 line art body and one caption line per control.

Each line keeps the end the original has at its caption (the labels are widgets and do not move) and reaches the Xbox
control the PlayStation button is bound to by default.
"""
from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

import sotb_texture as tex
import xbox_assets as assets
from sotb_pak import Pak

ROOT = Path(__file__).resolve().parents[2]
LINE_ART = ROOT / "assets" / "source" / "controller" / "xbox-series-lineart.png"
PANEL = assets.CONTENT + "Frontend/OptionsScreen/Textures/3DPanels/Controls/"

WIDTH, HEIGHT = 630, 480
SCALE = 0.55
ART_BOX = (11, 11, 1012, 681)  # the drawn part of the 1024x693 line art
ART_CENTER = (315.0, 240.0)
SS = 4
STROKE = 2.2
DOT = 3.6


def art_point(ax: float, ay: float) -> tuple[float, float]:
    left = ART_CENTER[0] - (ART_BOX[2] - ART_BOX[0]) * SCALE / 2
    top = ART_CENTER[1] - (ART_BOX[3] - ART_BOX[1]) * SCALE / 2
    return left + (ax - ART_BOX[0]) * SCALE, top + (ay - ART_BOX[1]) * SCALE


def _controls() -> dict[str, tuple[float, float]]:
    p = art_point
    return {
        "y": p(771, 155), "x": p(705, 222), "b": p(840, 222), "a": p(771, 288),
        "view": p(448, 214), "left_stick": p(243, 208), "right_stick": p(645, 370), "dpad_left": p(318, 372),
    }


def routes() -> dict[str, tuple[list[tuple[float, float]], tuple[float, float]]]:
    """Per line texture: the polyline from the caption end to the control, and the dot that marks the control."""
    c = _controls()
    lb = art_point(238, 62)
    rb = art_point(800, 62)
    lt = art_point(214, 14)
    rt = art_point(800, 14)
    out: dict[str, tuple[list[tuple[float, float]], tuple[float, float]]] = {}

    def add(name, points):
        out[name] = (points, points[-1])

    add("lineL2", [(0, 38), (lt[0], 38), (lt[0], lt[1] + 1)])
    add("lineL1", [(0, 84), (lb[0], 84), lb])
    add("lineR2", [(626, 38.5), (rt[0], 38.5), (rt[0], rt[1] + 1)])
    add("lineR1", [(626, 83.5), (rb[0], 83.5), rb])
    y, b, x, a = c["y"], c["b"], c["x"], c["a"]
    add("lineTriangle", [(626, 162.5), (535, 162.5), (535, y[1]), (y[0] + 12, y[1])])
    add("lineCircle", [(626, 241.5), (546, 241.5), (546, b[1]), (b[0] + 12, b[1])])
    add("lineCross", [(626, 316.5), (a[0], 316.5), (a[0], a[1] + 12)])
    add("lineSquare", [(626, 359.5), (608, 359.5), (608, 22), (x[0], 22), (x[0], x[1] - 20)])
    dl = c["dpad_left"]
    add("lineDpad", [(0, 241.5), (dl[0], 241.5)])
    ls, rs = c["left_stick"], c["right_stick"]
    add("lineLStick", [(0, 402.5), (ls[0], 402.5), ls])
    add("lineL3", [(0, 443.5), (ls[0] - 22, 443.5), (ls[0] - 22, ls[1] + 26)])
    add("lineRStick", [(626, 402.5), (rs[0], 402.5), rs])
    add("lineR3", [(626, 443.5), (rs[0] + 22, 443.5), (rs[0] + 22, rs[1] + 22)])
    view = c["view"]
    add("lineTouchpad", [(309, 69), (309, 80), (view[0], 80), (view[0], view[1] - 11)])
    return out


def _stroke_image(polylines: list[list[tuple[float, float]]], dots: list[tuple[float, float]]) -> np.ndarray:
    image = Image.new("L", (WIDTH * SS, HEIGHT * SS), 0)
    draw = ImageDraw.Draw(image)
    for points in polylines:
        scaled = [(px * SS, py * SS) for px, py in points]
        draw.line(scaled, fill=255, width=int(STROKE * SS), joint="curve")
        for px, py in scaled:
            r = STROKE * SS / 2
            draw.ellipse([px - r, py - r, px + r, py + r], fill=255)
    for px, py in dots:
        r = DOT * SS
        draw.ellipse([px * SS - r, py * SS - r, px * SS + r, py * SS + r], fill=255)
    return np.asarray(image.resize((WIDTH, HEIGHT), Image.BOX), dtype=np.uint8)


def _white_with_alpha(alpha: np.ndarray) -> np.ndarray:
    out = np.full((HEIGHT, WIDTH, 4), 255, dtype=np.uint8)
    out[..., 3] = alpha
    return out


def _outside(ink: np.ndarray) -> np.ndarray:
    """The pixels that reach the border of the image without crossing ink."""
    reached = np.zeros_like(ink)
    reached[0, :], reached[-1, :], reached[:, 0], reached[:, -1] = True, True, True, True
    reached &= ~ink
    while True:
        grown = reached.copy()
        grown[1:] |= reached[:-1]
        grown[:-1] |= reached[1:]
        grown[:, 1:] |= reached[:, :-1]
        grown[:, :-1] |= reached[:, 1:]
        grown &= ~ink
        if np.array_equal(grown, reached):
            return reached
        reached = grown


def controller_body() -> np.ndarray:
    art = Image.open(LINE_ART).convert("RGBA")
    ink = art.split()[3].crop(ART_BOX)
    size = (round(ink.width * SCALE), round(ink.height * SCALE))
    ink = ink.resize(size, Image.LANCZOS)
    ink_array = np.clip(np.asarray(ink, dtype=np.float64) * 1.55, 0, 255).astype(np.uint8)
    left = round(ART_CENTER[0] - size[0] / 2)
    top = round(ART_CENTER[1] - size[1] / 2)
    alpha = np.zeros((HEIGHT, WIDTH), dtype=np.uint8)
    alpha[top:top + size[1], left:left + size[0]] = ink_array
    outside_mask = _outside(alpha > 40)
    body = np.where(outside_mask, 0, 13).astype(np.uint8)
    return _white_with_alpha(np.maximum(alpha, body))


def diagram_replacements(pak: Pak) -> list[assets.Replacement]:
    out = []
    body = controller_body()
    jobs = {"controller": (body, "Controller Settings controller")}
    lines = routes()
    for name, (points, dot) in lines.items():
        jobs[name] = (_white_with_alpha(_stroke_image([points], [dot])), f"Controller Settings caption line {name[4:]}")
    ls, rs = _controls()["left_stick"], _controls()["right_stick"]
    both = ([[(311.5, 479), (311.5, 440)], [(ls[0], 440), (rs[0], 440)], [(ls[0], 440), (ls[0], ls[1] + 44)], [(rs[0], 440), (rs[0], rs[1] + 44)]],
            [(ls[0], ls[1] + 44), (rs[0], rs[1] + 44)])
    jobs["lineL3R3"] = (_white_with_alpha(_stroke_image(both[0], both[1])), "Controller Settings caption line L3R3")
    for name, (image, purpose) in jobs.items():
        full = PANEL + name
        original = pak.read(pak.entry(full + ".uasset"))
        texture = tex.parse_texture(original)
        if (texture.width, texture.height, len(texture.mips)) != (WIDTH, HEIGHT, 1) or texture.pixel_format != "PF_B8G8R8A8":
            raise ValueError(f"{full} is not the expected 630x480 BGRA texture")
        out.append(assets.Replacement(full, original, assets.write_mips(original, texture, image), texture, purpose))
    return out
