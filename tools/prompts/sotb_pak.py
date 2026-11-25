"""Read-only access to the unencrypted, uncompressed UE4 pak (version 3) files of Shadow of the Beast."""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

PAK_MAGIC = 0x5A6F12E1
FOOTER_SIZE = 44
ENTRY_HEADER_SIZE = 53

BUILD_KEY = "CUSA03762-1.00"
PAK0_RELATIVE = Path("beast/content/paks/pakchunk0-ps4.pak")
PAK0_GUEST = "/app0/beast/content/paks/pakchunk0-ps4.pak"


@dataclass(frozen=True)
class PakEntry:
    name: str
    offset: int
    size: int
    uncompressed_size: int
    compression: int
    encrypted: int

    @property
    def data_offset(self) -> int:
        return self.offset + ENTRY_HEADER_SIZE


@dataclass(frozen=True)
class PakIdentity:
    file_size: int
    version: int
    index_offset: int
    index_size: int
    index_crc32: int


BUILD_PAK_IDENTITIES: tuple[tuple[str, PakIdentity], ...] = (
    ("CUSA03762-1.00", PakIdentity(6634484704, 3, 6633116901, 1367759, 0x7259963B)),
    ("CUSA02871-1.00", PakIdentity(6902920160, 3, 6901552357, 1367759, 0xA1CD828C)),
    ("CUSA00951-1.00", PakIdentity(6902920160, 3, 6901552357, 1367759, 0xA1CD828C)),
)


def _fstring(blob: bytes, pos: int) -> tuple[str, int]:
    (length,) = struct.unpack_from("<i", blob, pos)
    pos += 4
    if length == 0:
        return "", pos
    if length > 0:
        return blob[pos:pos + length - 1].decode("utf-8", "replace"), pos + length
    length = -length
    return blob[pos:pos + 2 * (length - 1)].decode("utf-16-le", "replace"), pos + 2 * length


class Pak:
    def __init__(self, path: Path):
        self.path = Path(path)
        self._file = open(self.path, "rb")
        self._file.seek(0, 2)
        size = self._file.tell()
        self._file.seek(size - FOOTER_SIZE)
        magic, version, index_offset, index_size = struct.unpack("<IIQQ", self._file.read(24))
        if magic != PAK_MAGIC or version != 3:
            raise ValueError("not a version 3 pak")
        if index_offset + index_size > size - FOOTER_SIZE:
            raise ValueError("pak index is outside the file")
        self._file.seek(index_offset)
        index = self._file.read(index_size)
        self.identity = PakIdentity(size, version, index_offset, index_size, zlib.crc32(index) & 0xFFFFFFFF)
        self.entries = self._parse_index(index)
        self._by_name = {entry.name: entry for entry in self.entries}

    @staticmethod
    def _parse_index(index: bytes) -> list[PakEntry]:
        _mount, pos = _fstring(index, 0)
        (count,) = struct.unpack_from("<i", index, pos)
        pos += 4
        entries = []
        for _ in range(count):
            name, pos = _fstring(index, pos)
            offset, size, usize, compression = struct.unpack_from("<QQQI", index, pos)
            pos += 28 + 20
            if compression:
                (blocks,) = struct.unpack_from("<i", index, pos)
                pos += 4 + 16 * blocks
            encrypted = index[pos]
            pos += 1 + 4
            entries.append(PakEntry(name, offset, size, usize, compression, encrypted))
        return entries

    def close(self) -> None:
        self._file.close()

    def __enter__(self) -> "Pak":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def entry(self, name: str) -> PakEntry:
        try:
            entry = self._by_name[name]
        except KeyError:
            raise KeyError(f"{name} is not in the pak") from None
        if entry.compression or entry.encrypted or entry.size != entry.uncompressed_size:
            raise ValueError(f"{name} is compressed or encrypted")
        return entry

    def read(self, entry: PakEntry) -> bytes:
        self._file.seek(entry.offset)
        header = self._file.read(ENTRY_HEADER_SIZE)
        _offset, size, usize, compression = struct.unpack_from("<QQQI", header, 0)
        if (size, usize, compression) != (entry.size, entry.uncompressed_size, 0):
            raise ValueError(f"{entry.name} has an unexpected in-pak header")
        data = self._file.read(entry.size)
        if len(data) != entry.size:
            raise ValueError(f"{entry.name} is truncated")
        return data


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF
