"""Cooked UE4.8 PS4 Texture2D packages: parsing, 1D thin micro-tiling and BC1, BC3 and BGRA8 codecs."""
from __future__ import annotations

import re
import struct
from dataclasses import dataclass

import numpy as np

FORMATS = {"PF_DXT5": (16, 4), "PF_DXT1": (8, 4), "PF_B8G8R8A8": (4, 1)}
TAIL_TAG = b"\xc1\x83\x2a\x9e"
BULK_INLINE = 0x40
BULK_AT_END = 0x01


@dataclass(frozen=True)
class Mip:
    width: int
    height: int
    size: int
    flags: int
    start: int  # byte offset of the payload inside the package


@dataclass(frozen=True)
class Texture:
    pixel_format: str
    width: int
    height: int
    mips: tuple[Mip, ...]

    @property
    def element_bytes(self) -> int:
        return FORMATS[self.pixel_format][0]

    @property
    def block(self) -> int:
        return FORMATS[self.pixel_format][1]


def _texture_at(package: bytes, fmt_pos: int, fmt: str) -> Texture:
    prefix = fmt_pos - 4
    width, height, slices = struct.unpack_from("<iii", package, prefix - 12)
    if slices != 1 or not 0 < width <= 8192 or not 0 < height <= 8192:
        raise ValueError("unsupported texture")
    pos = fmt_pos + len(fmt) + 1
    first, count = struct.unpack_from("<ii", package, pos)
    if first != 0 or not 1 <= count <= 24:
        raise ValueError("unsupported mip table")
    pos += 8
    element, block = FORMATS[fmt]
    raw = []
    for _ in range(count):
        cooked, flags, element_count, size, bulk_offset = struct.unpack_from("<iiiiq", package, pos)
        pos += 24
        if cooked != 1 or element_count != size or size <= 0:
            raise ValueError("unexpected mip header")
        at_end = bool(flags & BULK_AT_END)
        if flags & BULK_INLINE and not at_end:
            start = pos
            pos += size
        elif at_end:
            start = bulk_offset
        else:
            raise ValueError("unsupported bulk data flags")
        mip_w, mip_h = struct.unpack_from("<ii", package, pos)
        pos += 8
        if not 0 < mip_w <= width or not 0 < mip_h <= height:
            raise ValueError("unexpected mip size")
        columns, rows = (mip_w + block - 1) // block, (mip_h + block - 1) // block
        if size != padded_elements(columns, rows) * element:
            raise ValueError("unexpected mip payload")
        raw.append((mip_w, mip_h, size, flags, start, at_end))
    base = len(package) - 4 - sum(m[2] for m in raw if m[5])
    mips = []
    for mip_w, mip_h, size, flags, start, at_end in raw:
        absolute = base + start if at_end else start
        if absolute < 0 or absolute + size > len(package) - (4 if at_end else 0):
            raise ValueError("mip payload is outside the package")
        mips.append(Mip(mip_w, mip_h, size, flags, absolute))
    return Texture(fmt, width, height, tuple(mips))


def textures(package: bytes) -> list[Texture]:
    """Every Texture2D serialized in a package, including the font page textures embedded in a font package."""
    if package[-4:] != TAIL_TAG:
        raise ValueError("package does not end with the file tag")
    (header,) = struct.unpack_from("<i", package, 0x18)
    out = []
    for match in re.finditer(rb"PF_[A-Za-z0-9_]+\x00", package[header:]):
        fmt_pos = match.start() + header
        (length,) = struct.unpack_from("<i", package, fmt_pos - 4)
        if length != len(match.group()):
            continue
        fmt = match.group()[:-1].decode()
        if fmt not in FORMATS:
            continue
        try:
            out.append(_texture_at(package, fmt_pos, fmt))
        except (ValueError, struct.error):
            continue
    if not out:
        raise ValueError("no readable texture")
    return out


def parse_texture(package: bytes) -> Texture:
    return textures(package)[0]


def _morton8(x: np.ndarray, y: np.ndarray) -> np.ndarray:
    return (x & 1) | ((y & 1) << 1) | (((x >> 1) & 1) << 2) | (((y >> 1) & 1) << 3) | (((x >> 2) & 1) << 4) | (((y >> 2) & 1) << 5)


def tiled_index(columns: int, rows: int) -> np.ndarray:
    """For each linear element (row-major, columns x rows) its position in the 1D thin tiled order."""
    padded_columns = (columns + 7) // 8 * 8
    y, x = np.mgrid[0:rows, 0:columns]
    tile = (y // 8) * (padded_columns // 8) + (x // 8)
    return tile * 64 + _morton8(x % 8, y % 8)


def padded_elements(columns: int, rows: int) -> int:
    return (columns + 7) // 8 * 8 * ((rows + 7) // 8 * 8)


def untile(data: bytes, columns: int, rows: int, element: int) -> np.ndarray:
    count = padded_elements(columns, rows)
    source = np.frombuffer(data[:count * element], dtype=np.uint8).reshape(count, element)
    return source[tiled_index(columns, rows)]


def tile(elements: np.ndarray, columns: int, rows: int, element: int, fill: np.ndarray | None = None) -> bytes:
    count = padded_elements(columns, rows)
    out = np.zeros((count, element), dtype=np.uint8) if fill is None else np.tile(fill, (count, 1)).astype(np.uint8)
    out[tiled_index(columns, rows)] = elements.reshape(rows, columns, element)
    return out.tobytes()


def _expand565(value: np.ndarray) -> np.ndarray:
    r = (value >> 11) & 31
    g = (value >> 5) & 63
    b = value & 31
    return np.stack([(r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31], -1)


def decode_bc3(data: bytes, width: int, height: int) -> np.ndarray:
    bw, bh = (width + 3) // 4, (height + 3) // 4
    blocks = np.frombuffer(data[:bw * bh * 16], dtype=np.uint8).reshape(bh, bw, 16).astype(np.int32)
    out = np.zeros((bh * 4, bw * 4, 4), dtype=np.uint8)
    out[..., :3] = _decode_color(blocks[..., 8:], bw, bh)[..., :3]
    out[..., 3] = _decode_alpha(blocks[..., :8], bw, bh)
    return out[:height, :width]


def _decode_alpha(blocks: np.ndarray, bw: int, bh: int) -> np.ndarray:
    a0, a1 = blocks[..., 0], blocks[..., 1]
    bits = np.zeros((bh, bw), dtype=np.uint64)
    for i in range(6):
        bits |= blocks[..., 2 + i].astype(np.uint64) << np.uint64(8 * i)
    palette = np.zeros((bh, bw, 8), dtype=np.int32)
    palette[..., 0], palette[..., 1] = a0, a1
    eight = a0 > a1
    for i in range(1, 7):
        seven = ((7 - i) * a0 + i * a1) // 7
        five = ((5 - i) * a0 + i * a1) // 5 if i < 5 else np.where(i == 5, 0, 255)
        palette[..., i + 1] = np.where(eight, seven, five)
    out = np.zeros((bh * 4, bw * 4), dtype=np.uint8)
    for p in range(16):
        index = ((bits >> np.uint64(3 * p)) & np.uint64(7)).astype(np.int64)
        y, x = divmod(p, 4)
        out[y::4, x::4] = np.take_along_axis(palette, index[..., None], -1)[..., 0]
    return out


def _decode_color(blocks: np.ndarray, bw: int, bh: int, four_color_only: bool = True) -> np.ndarray:
    c0 = blocks[..., 0] | (blocks[..., 1] << 8)
    c1 = blocks[..., 2] | (blocks[..., 3] << 8)
    e0, e1 = _expand565(c0), _expand565(c1)
    four = np.ones_like(c0, dtype=bool) if four_color_only else (c0 > c1)
    p2 = np.where(four[..., None], (2 * e0 + e1) // 3, (e0 + e1) // 2)
    p3 = np.where(four[..., None], (e0 + 2 * e1) // 3, 0)
    palette = np.stack([e0, e1, p2, p3], -2)
    bits = np.zeros((bh, bw), dtype=np.uint32)
    for i in range(4):
        bits |= blocks[..., 4 + i].astype(np.uint32) << np.uint32(8 * i)
    out = np.zeros((bh * 4, bw * 4, 4), dtype=np.uint8)
    out[..., 3] = 255
    for p in range(16):
        index = ((bits >> np.uint32(2 * p)) & 3).astype(np.int64)
        y, x = divmod(p, 4)
        out[y::4, x::4, :3] = np.take_along_axis(palette, index[..., None, None], -2)[..., 0, :]
        if not four_color_only:
            out[y::4, x::4, 3] = np.where(~four & (index == 3), 0, 255)
    return out


def decode_bc1(data: bytes, width: int, height: int) -> np.ndarray:
    bw, bh = (width + 3) // 4, (height + 3) // 4
    blocks = np.frombuffer(data[:bw * bh * 8], dtype=np.uint8).reshape(bh, bw, 8).astype(np.int32)
    return _decode_color(blocks, bw, bh, four_color_only=False)[:height, :width]


def _gather_blocks(image: np.ndarray) -> np.ndarray:
    height, width = image.shape[:2]
    bh, bw = (height + 3) // 4, (width + 3) // 4
    padded = np.zeros((bh * 4, bw * 4, image.shape[2]), dtype=image.dtype)
    padded[:height, :width] = image
    padded[height:, :width] = padded[height - 1:height, :width]
    padded[:, width:] = padded[:, width - 1:width]
    return padded.reshape(bh, 4, bw, 4, -1).transpose(0, 2, 1, 3, 4).reshape(bh * bw, 16, -1)


def _to565(color: np.ndarray) -> np.ndarray:
    c = np.clip(np.rint(color), 0, 255).astype(np.int32)
    return ((c[..., 0] * 31 + 127) // 255 << 11) | ((c[..., 1] * 63 + 127) // 255 << 5) | ((c[..., 2] * 31 + 127) // 255)


def _encode_color(pixels: np.ndarray, weight: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """pixels (n,16,3) float, weight (n,16) 0..1. Returns the 4-byte endpoints and 4-byte indices per block."""
    n = pixels.shape[0]
    total = np.maximum(weight.sum(1, keepdims=True), 1e-6)
    mean = (pixels * weight[..., None]).sum(1, keepdims=True) / total[..., None]
    centered = (pixels - mean) * weight[..., None]
    cov = np.einsum("npi,npj->nij", centered, pixels - mean)
    axis = np.tile(np.array([0.57735, 0.57735, 0.57735]), (n, 1))
    for _ in range(6):
        axis = np.einsum("nij,nj->ni", cov, axis)
        norm = np.linalg.norm(axis, axis=1, keepdims=True)
        axis = np.where(norm > 1e-9, axis / np.maximum(norm, 1e-9), np.array([0.57735, 0.57735, 0.57735]))
    projection = np.einsum("npi,ni->np", pixels - mean, axis)
    valid = weight > 0.02
    low = np.where(valid, projection, np.inf).min(1)
    high = np.where(valid, projection, -np.inf).max(1)
    empty = ~valid.any(1)
    low = np.where(empty, 0, low)
    high = np.where(empty, 0, high)
    ends = []
    for t in (high, low):
        ends.append(np.clip(mean[:, 0, :] + axis * t[:, None], 0, 255))
    best_indices = None
    for _ in range(3):
        c0, c1 = _to565(ends[0]), _to565(ends[1])
        swap = c0 < c1
        c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
        e0, e1 = _expand565(c0).astype(np.float64), _expand565(c1).astype(np.float64)
        palette = np.stack([e0, e1, (2 * e0 + e1) / 3, (e0 + 2 * e1) / 3], 1)
        distance = ((pixels[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(-1)
        index = distance.argmin(2)
        best_indices = index
        # Least squares refit of the endpoints for the chosen indices.
        t = np.array([1.0, 0.0, 2 / 3, 1 / 3])[index] * weight
        s = (1 - np.array([1.0, 0.0, 2 / 3, 1 / 3])[index]) * weight
        a11 = (t * t).sum(1)
        a22 = (s * s).sum(1)
        a12 = (t * s).sum(1)
        det = a11 * a22 - a12 * a12
        ok = np.abs(det) > 1e-6
        b1 = np.einsum("np,npi->ni", t, pixels)
        b2 = np.einsum("np,npi->ni", s, pixels)
        safe = np.where(ok, det, 1.0)[:, None]
        n0 = (a22[:, None] * b1 - a12[:, None] * b2) / safe
        n1 = (a11[:, None] * b2 - a12[:, None] * b1) / safe
        ends = [np.where(ok[:, None], np.clip(n0, 0, 255), ends[0]), np.where(ok[:, None], np.clip(n1, 0, 255), ends[1])]
    c0, c1 = _to565(ends[0]), _to565(ends[1])
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    e0, e1 = _expand565(c0).astype(np.float64), _expand565(c1).astype(np.float64)
    palette = np.stack([e0, e1, (2 * e0 + e1) / 3, (e0 + 2 * e1) / 3], 1)
    index = ((pixels[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(-1).argmin(2)
    same = c0 == c1
    index = np.where(same[:, None], 0, index)
    endpoints = np.stack([c0 & 255, c0 >> 8, c1 & 255, c1 >> 8], 1).astype(np.uint8)
    bits = np.zeros(n, dtype=np.uint64)
    for p in range(16):
        bits |= index[:, p].astype(np.uint64) << np.uint64(2 * p)
    packed = np.stack([(bits >> np.uint64(8 * i)) & np.uint64(255) for i in range(4)], 1).astype(np.uint8)
    return endpoints, packed


def encode_bc1(image: np.ndarray, selection: np.ndarray | None = None) -> bytes:
    """Opaque RGB(A) image to BC1 in raster block order, or only the selected blocks; blocks are always in four-colour mode."""
    pixels = _gather_blocks(image[..., :3])
    if selection is not None:
        pixels = pixels[selection]
    pixels = pixels.astype(np.float64)
    weight = np.ones(pixels.shape[:2])
    endpoints, indices = _encode_color(pixels, weight)
    return np.concatenate([endpoints, indices], 1).tobytes()


def encode_bc3(image: np.ndarray) -> bytes:
    blocks = _gather_blocks(image).astype(np.float64)
    alpha = blocks[..., 3]
    weight = alpha / 255.0
    endpoints, color_indices = _encode_color(blocks[..., :3], weight)
    a_hi = alpha.max(1).astype(np.int32)
    a_lo = alpha.min(1).astype(np.int32)
    flat = a_hi == a_lo
    a0 = np.where(flat, np.minimum(a_hi + 1, 255), a_hi)
    a1 = np.where(flat, np.minimum(a_lo, a0 - 1).clip(0), a_lo)
    a0 = np.where(a0 == a1, np.minimum(a0 + 1, 255), a0)
    a1 = np.where(a0 == a1, a0 - 1, a1)
    palette = np.stack([a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)], 1).astype(np.float64)
    index = np.abs(alpha[:, :, None] - palette[:, None, :]).argmin(2)
    bits = np.zeros(alpha.shape[0], dtype=np.uint64)
    for p in range(16):
        bits |= index[:, p].astype(np.uint64) << np.uint64(3 * p)
    alpha_bytes = np.stack([a0, a1] + [((bits >> np.uint64(8 * i)) & np.uint64(255)).astype(np.int64) for i in range(6)], 1).astype(np.uint8)
    return np.concatenate([alpha_bytes, endpoints, color_indices], 1).tobytes()


def decode_mip(package: bytes, texture: Texture, level: int) -> np.ndarray:
    mip = texture.mips[level]
    element = texture.element_bytes
    block = texture.block
    columns, rows = (mip.width + block - 1) // block, (mip.height + block - 1) // block
    linear = untile(package[mip.start:mip.start + mip.size], columns, rows, element).tobytes()
    if texture.pixel_format == "PF_DXT5":
        return decode_bc3(linear, mip.width, mip.height)
    if texture.pixel_format == "PF_DXT1":
        return decode_bc1(linear, mip.width, mip.height)
    pixels = np.frombuffer(linear, dtype=np.uint8).reshape(rows, columns, 4)[..., [2, 1, 0, 3]]
    return pixels[:mip.height, :mip.width].copy()


def encode_mip(texture: Texture, level: int, image: np.ndarray, solid_tail: bool = False) -> bytes:
    """Tiled payload for one mip. A mip of at most 4x4 texels is written as its average colour in every element."""
    mip = texture.mips[level]
    element = texture.element_bytes
    block = texture.block
    columns, rows = (mip.width + block - 1) // block, (mip.height + block - 1) // block
    if texture.pixel_format == "PF_B8G8R8A8":
        linear = np.ascontiguousarray(image[..., [2, 1, 0, 3]]).reshape(-1, 4)
    elif texture.pixel_format == "PF_DXT5":
        linear = np.frombuffer(encode_bc3(image), dtype=np.uint8).reshape(-1, 16)
    else:
        linear = np.frombuffer(encode_bc1(image), dtype=np.uint8).reshape(-1, 8)
    fill = None
    if solid_tail:
        fill = linear[:1]
    payload = tile(linear, columns, rows, element, fill=fill)
    if len(payload) != mip.size:
        raise ValueError("encoded mip does not fit its slot")
    return payload
