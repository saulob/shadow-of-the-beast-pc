#!/usr/bin/env python3
"""Static import coverage audit for Shadow of the Beast.

Reads the prepared boot image (never runs it) and classifies every imported
NID against the resolver tables in this tree, then against the four Saulo
reference ports when an import is missing.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
REFERENCES = [
    ("Dragon", Path(r"S:\VisualStudio\repos\dragons-crown-pro-pc")),
    ("Odin", Path(r"S:\VisualStudio\repos\odin-sphere-leifthrasir-pc")),
    ("13S", Path(r"S:\VisualStudio\repos\13-sentinels-aegis-rim-pc")),
    ("Alienation", Path(r"S:\VisualStudio\repos\alienation-pc")),
]

# Priority groups from the bring-up plan: early boot, graphics, audio, movies, services.
# Anything not listed is a service call the game can survive without until it is actually used.
LIBRARY_GROUPS = {
    "libc": "A", "libSceLibcInternal": "A", "libkernel": "A", "libScePosix": "A",
    "libSceFios2": "A", "libSceUserService": "A", "libSceSystemService": "A",
    "libScePad": "A", "libSceRtc": "A", "libSceSaveData": "A", "libScePlayGo": "A",
    "libSceAppContent": "A", "libSceSysmodule": "A", "libSceCommonDialog": "A",
    "libSceMsgDialog": "A", "libSceImeDialog": "A", "libSceSaveDataDialog": "A",
    "libSceErrorDialog": "A", "libSceDiscMap": "A", "libSceCoredump": "A",
    "libSceNetCtl": "A",
    "libSceGnmDriver": "B", "libSceVideoOut": "B",
    "libSceAudioOut": "C", "libSceAudio3d": "C", "libSceAjm": "C",
    "libSceNgs2": "C", "libSceAvPlayer": "D", "libSceVideodec": "D",
}

CATEGORY_RANK = {"IMPLEMENTED": 0, "ALIAS": 1, "SAFE_STUB": 2, "MISSING_OPTIONAL": 3,
                 "MISSING_REQUIRED": 4, "UNKNOWN": 5}


@dataclass
class Import:
    index: int
    scoped: str
    nid: str
    library: str
    kind: int
    symbol: str = ""
    category: str = "UNKNOWN"
    target: str = ""
    callsites: int = 0
    references: list[str] = field(default_factory=list)


def read64(data: bytes, offset: int) -> tuple[int, int]:
    return struct.unpack_from("<Q", data, offset)[0], offset + 8


def parse_boot(path: Path) -> tuple[dict, list[Import], set[int], dict[int, int], bytes, int]:
    data = path.read_bytes()
    if data[:8] == b"SOTBBOT1":
        offset = 8
    elif data[:9] == b"SOTBBOOT1":
        offset = 9  # boot images written before the magic was shortened to the 8 bytes the loader reads
    else:
        raise SystemExit(f"{path}: not a SOTBBOT1 boot image")
    fields = {}
    for name in ("size", "entry", "segments", "relocations", "imports", "capabilities"):
        fields[name], offset = read64(data, offset)
    fields["procparam"], offset = read64(data, offset)
    tls = []
    for _ in range(4):
        value, offset = read64(data, offset)
        tls.append(value)
    fields["tls"] = tls
    module_count, offset = read64(data, offset)
    for _ in range(module_count):
        offset += 7 * 8
    binding_count, offset = read64(data, offset)
    bindings = {}
    for _ in range(binding_count):
        index, offset = read64(data, offset)
        address, offset = read64(data, offset)
        kind, offset = read64(data, offset)
        bindings[index] = (address, kind)
    offset += fields["segments"] * 3 * 8
    names = []
    for _ in range(fields["imports"]):
        raw = data[offset:offset + 128]
        offset += 128
        names.append(raw.split(b"\0", 1)[0].decode("utf-8", "replace"))
    relocations = []
    for _ in range(fields["relocations"]):
        target, kind, value, addend = struct.unpack_from("<QQQQ", data, offset)
        offset += 32
        if kind:
            relocations.append((value, kind, target))
    kinds = {}
    iat_targets = {}
    for value, kind, target in relocations:
        kinds.setdefault(value, kind)
        if kind == 1:
            iat_targets[target] = value
    image_offset = offset
    image = data[image_offset:image_offset + fields["size"]]
    imports = []
    for index, scoped in enumerate(names):
        nid, _, library = scoped.partition("#")
        imports.append(Import(index, scoped, nid, library, kinds.get(index, 0)))
    return fields, imports, bindings, iat_targets, image, image_offset


def bundled_modules(root: Path) -> list[str]:
    text = (root / "launcher/game_profile.h").read_text(encoding="utf-8", errors="replace")
    block = re.search(r"kBundledModules\[\]\s*=\s*\{([^}]*)\}", text)
    return re.findall(r'"([^"]+)"', block.group(1)) if block else []


def boot_layout(path: Path) -> tuple[list[tuple[int, int]], list[str], dict[int, tuple[int, int, int]]]:
    """Linked module ranges, import names and every relocation keyed by target, as the loader reads them."""
    data = path.read_bytes()
    offset = 8 if data[:8] == b"SOTBBOT1" else 9
    header = struct.unpack_from("<6Q", data, offset)
    offset += 6 * 8 + 8 + 4 * 8
    module_count = struct.unpack_from("<Q", data, offset)[0]
    offset += 8
    modules = [struct.unpack_from("<2Q", data, offset + m * 56) for m in range(module_count)]
    offset += module_count * 56
    offset += 8 + struct.unpack_from("<Q", data, offset)[0] * 24 + header[2] * 24
    names = [data[offset + i * 128:offset + i * 128 + 128].split(b"\0", 1)[0].decode() for i in range(header[4])]
    offset += header[4] * 128
    relocations = {}
    for i in range(header[3]):
        target, kind, value, addend = struct.unpack_from("<4Q", data, offset + i * 32)
        if target in relocations:
            raise SystemExit(f"duplicate relocation target 0x{target:x}")
        relocations[target] = (kind, value, addend)
    return modules, names, relocations


def audit_slots(boot: Path, game: Path) -> int:
    """Proves each import slot of the boot image against the dynamic symbol of the original ELF that owns it."""
    import ps4_module

    modules, names, relocations = boot_layout(boot)
    files = bundled_modules(ROOT)
    if len(files) != len(modules):
        raise SystemExit(f"boot image has {len(modules)} linked modules, launcher/game_profile.h lists {len(files)}")
    units = [("eboot", game / "eboot.bin", 0)]
    units += [(f"linked module {m} ({name})", game / "sce_module" / name, base)
              for m, (name, (base, _size)) in enumerate(zip(files, modules))]
    errors = 0
    for label, path, base in units:
        module = ps4_module.read_module(path)
        slots = code = data = bad = 0
        used = set()
        for target, kind, symbol_index, addend in module["relocs"]:
            if kind not in (1, 6, 7):
                continue
            symbol = module["symbols"][symbol_index]
            if symbol["section"] or not symbol["identity"]:
                continue
            want_name = ps4_module.canonical(symbol["identity"])
            want_kind = 2 if symbol["type"] == 1 else 1
            got = relocations.get(base + target)
            slots += 1
            if not got or got[0] != want_kind or names[got[1]] != want_name or got[2] != addend:
                bad += 1
                if bad <= 16:
                    print(f"  SLOT MISMATCH {label} 0x{base + target:x}: ELF {want_name} kind {want_kind} addend {addend}, "
                          f"boot {got and (got[0], names[got[1]] if got[0] else hex(got[1]), got[2])}")
                continue
            used.add(got[1])
            code += want_kind == 1
            data += want_kind == 2
        errors += bad
        span = f"{min(used)}..{max(used)}" if used else "-"
        print(f"{label}: {slots} import slots ({code} code, {data} data), {len(used)} imports, "
              f"global indices {span}, mismatches {bad}")
    kinds = {}
    for kind, value, _addend in relocations.values():
        if kind:
            kinds.setdefault(value, set()).add(kind)
    mixed = [names[i] for i, k in kinds.items() if len(k) > 1]
    print(f"slots: {sum(1 for r in relocations.values() if r[0])}, imports with both code and data slots: {len(mixed)}, "
          f"duplicate names: {len(names) - len(set(names))}")
    return errors + len(mixed) + len(names) - len(set(names))


def prove_callsite(boot: Path, image: bytes, offset: int) -> None:
    """call rel32 / call [rip] ending at the return offset -> PLT thunk -> GOT slot -> import."""
    _modules, names, relocations = boot_layout(boot)
    slot = thunk = None
    if image[offset - 5] == 0xE8:
        target = offset + struct.unpack_from("<i", image, offset - 4)[0]
        if image[target:target + 2] == bytes((0xFF, 0x25)):
            thunk, slot = target, target + 6 + struct.unpack_from("<i", image, target + 2)[0]
    if slot is None and image[offset - 6:offset - 4] == bytes((0xFF, 0x15)):
        slot = offset + struct.unpack_from("<i", image, offset - 4)[0]
    if slot is None:
        print(f"return offset 0x{offset:x}: no direct import call ends here")
        return
    kind, value, addend = relocations.get(slot, (None, None, None))
    via = f"PLT thunk 0x{thunk:x} -> " if thunk is not None else ""
    if not kind:
        print(f"return offset 0x{offset:x}: {via}GOT slot 0x{slot:x} is not an import slot")
        return
    print(f"return offset 0x{offset:x}: {via}GOT slot 0x{slot:x} -> import {value} {names[value]} "
          f"({import_names(ROOT).get(names[value], '?')}), {'data' if kind == 2 else 'code'}, addend {addend}")


def count_callsites(image: bytes, iat_targets: dict[int, int]) -> dict[int, int]:
    """Calls that reach an import: direct `call [rip+iat]` and `call` to its `jmp [rip+iat]` thunk."""
    thunks = {}
    for i in range(1, len(image) - 6):
        if image[i] != 0xFF or image[i + 1] != 0x25:
            continue
        length = 7 if 0x40 <= image[i - 1] <= 0x4F else 6
        disp = struct.unpack_from("<i", image, i + 2)[0]
        index = iat_targets.get(i + length + disp)
        if index is not None:
            thunks.setdefault(i, index)
    counts = {}
    for i in range(1, len(image) - 6):
        if image[i] == 0xFF and image[i + 1] == 0x15:
            length = 7 if 0x40 <= image[i - 1] <= 0x4F else 6
            disp = struct.unpack_from("<i", image, i + 2)[0]
            index = iat_targets.get(i + length + disp)
            if index is not None:
                counts[index] = counts.get(index, 0) + 1
        elif image[i] == 0xE8:
            target = i + 5 + struct.unpack_from("<i", image, i + 1)[0]
            index = thunks.get(target)
            if index is not None:
                counts[index] = counts.get(index, 0) + 1
    return counts


SAFE_STUB_TARGETS = {
    "ok_noop", "null_pointer", "not_found", "dialog_unavailable", "dialog_none", "http_network",
    "net_unreachable", "kernel_chmod", "kernel_utimes",
    "posix_socket", "posix_socket_op", "posix_listen", "posix_accept", "posix_send", "posix_recv",
    "posix_sendto", "posix_recvfrom", "posix_sockname_op", "posix_sockopt", "posix_shutdown", "posix_fcntl",
    "posix_select", "net_init", "net_term", "netctl_init", "netctl_term", "netctl_result", "netctl_state",
    "netctl_info", "netctl_check", "netctl_unregister", "netctl_nat",
}


def import_names(root: Path) -> dict[str, str]:
    text = (root / "src/import_names.inc").read_text(encoding="utf-8", errors="replace")
    return dict(re.findall(r'\{"([^"]+)"\s*,\s*"([^"]+)"\}', text))


def exact_nids(root: Path) -> tuple[set[str], set[str], set[str]]:
    """(code NIDs, data NIDs, GPU NIDs matched by NID alone). Only resolver tables count."""
    runtime_text = (root / "src/runtime.c").read_text(encoding="utf-8", errors="replace")
    data_block = re.search(r"if \(is_data\) \{(.*?)return 0;\s*\}", runtime_text, re.S)
    data_nids = set(re.findall(r'"([A-Za-z0-9+\-_]{8,16}#lib[A-Za-z0-9]+)"', data_block.group(1))) if data_block else set()
    code_nids = set()
    for path in (root / "src").glob("*.c"):
        if path.name in ("loader.c", "runtime_log.c", "runtime_session_log.c"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        code_nids.update(re.findall(r'strcmp\([^,]+,\s*"([A-Za-z0-9+\-_]{8,16}#lib[A-Za-z0-9]+)"', text))
        code_nids.update(re.findall(r'\{\s*"([A-Za-z0-9+\-_]{8,16}#lib[A-Za-z0-9]+)"\s*,', text))
    gpu_nids = set()
    for path in (root / "gpu").glob("**/*.cpp"):
        if "third_party" in path.parts:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        gpu_nids.update(re.findall(r'LIB_(?:FUNCTION|OBJ)\w*\(\s*"([A-Za-z0-9+\-_]{8,16})"', text))
    return code_nids - data_nids, data_nids, gpu_nids


def exported_symbols(root: Path) -> dict[str, str]:
    symbols = {}
    for path in (root / "src").glob("*.c"):
        text = path.read_text(encoding="utf-8", errors="replace")
        for block in re.findall(r'RuntimeExport\s+\w+\s*\[\s*\]\s*=\s*\{(.*?)\n\s*\}\s*;', text, re.S):
            for symbol, target in re.findall(r'\{\s*"([^"]+)"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}', block):
                symbols[symbol] = target
    return symbols


def reference_index() -> dict[str, tuple[set[str], set[str]]]:
    index = {}
    for name, root in REFERENCES:
        if not root.is_dir():
            continue
        text_parts = []
        for pattern in ("src/**/*.c", "src/**/*.h", "launcher/**/*.cpp", "launcher/**/*.h",
                        "gpu/shim/**/*.cpp", "gpu/shim/**/*.h"):
            for path in root.glob(pattern):
                if "third_party" in path.parts:
                    continue
                text_parts.append(path.read_text(encoding="utf-8", errors="replace"))
        text = "\n".join(text_parts)
        index[name] = (set(re.findall(r'[A-Za-z0-9+\-_]{8,16}#lib[A-Za-z0-9]+', text)),
                       set(re.findall(r'"([A-Za-z_][A-Za-z0-9_]{3,})"', text)))
    return index


def classify(imports: list[Import], fields: dict, bindings: dict[int, tuple[int, int]], nids: tuple[set[str], set[str], set[str]],
             symbols: dict[str, str], names: dict[str, str], refs: dict[str, tuple[set[str], set[str]]]) -> None:
    code_nids, data_nids, gpu_nids = nids
    for item in imports:
        canonical = item.scoped
        if item.library == "libScePosix":
            canonical = item.nid + "#libkernel"
        item.symbol = names.get(canonical, "") or names.get(item.scoped, "")
        if not (fields["capabilities"] & 1):
            item.category = "MISSING_REQUIRED"
            continue
        binding = bindings.get(item.index)
        if binding and binding[1] == item.kind:
            item.category = "IMPLEMENTED"
            item.target = "native binding"
            continue
        if item.kind == 2:
            if canonical in data_nids or item.scoped in data_nids:
                item.category = "IMPLEMENTED"
                item.target = "data export"
            else:
                item.category = "MISSING_REQUIRED" if library_group(item.library) in "AB" else "MISSING_OPTIONAL"
            continue
        if canonical in code_nids or item.nid in gpu_nids:
            item.category = "ALIAS" if canonical != item.scoped else "IMPLEMENTED"
            item.target = "exact NID" if canonical == item.scoped else f"alias {canonical}"
            continue
        if item.symbol and item.symbol in symbols:
            target = symbols[item.symbol]
            item.target = f"export {item.symbol} -> {target}"
            item.category = "SAFE_STUB" if target in SAFE_STUB_TARGETS else "IMPLEMENTED"
            continue
        group = library_group(item.library)
        item.category = "MISSING_OPTIONAL" if group in "CDE" else "MISSING_REQUIRED"
        if not item.symbol:
            item.category = "UNKNOWN"
        for name, (ref_nids, ref_symbols) in refs.items():
            if item.scoped in ref_nids or item.nid in ref_nids or (item.symbol and item.symbol in ref_symbols):
                item.references.append(name)


def library_group(library: str) -> str:
    return LIBRARY_GROUPS.get(library, "E")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot", type=Path, default=ROOT / "build/image/CUSA03762-1.00/boot.bin")
    parser.add_argument("--json", type=Path)
    parser.add_argument("--references", action="store_true", help="cross-check missing imports")
    parser.add_argument("--missing", action="store_true", help="print the missing imports")
    parser.add_argument("--slots", action="store_true",
                        help="prove every import slot against the original eboot and linked module ELFs")
    parser.add_argument("--game", type=Path, default=ROOT / "game/prepared/CUSA03762-1.00")
    parser.add_argument("--callsite", type=lambda text: int(text, 0), action="append", default=[],
                        help="caller return offset to resolve to its import")
    args = parser.parse_args()

    if args.slots or args.callsite:
        fields, _imports, _bindings, _iat, image, _offset = parse_boot(args.boot)
        for offset in args.callsite:
            prove_callsite(args.boot, image, offset)
        if args.slots:
            errors = audit_slots(args.boot, args.game)
            print(f"import slot audit: {'FAILED' if errors else 'passed'}")
            return 1 if errors else 0
        return 0

    fields, imports, bindings, iat_targets, image, image_offset = parse_boot(args.boot)
    names = import_names(ROOT)
    refs = reference_index() if args.references else {}
    classify(imports, fields, bindings, exact_nids(ROOT), exported_symbols(ROOT), names, refs)
    calls = count_callsites(image, iat_targets)
    for item in imports:
        item.callsites = calls.get(item.index, 0)

    counts = {}
    for item in imports:
        counts[item.category] = counts.get(item.category, 0) + 1
    libraries = {}
    for item in imports:
        libraries[item.library] = libraries.get(item.library, 0) + 1
    print(f"boot image: {args.boot}")
    print(f"imports: {len(imports)}  libraries: {len(libraries)}  capabilities: {fields['capabilities']}")
    print(f"entry: 0x{fields['entry']:x}  image: 0x{image_offset:x}..0x{image_offset + len(image):x}")
    unused = sum(1 for item in imports if item.callsites == 0)
    print(f"call sites: {sum(calls.values())}  imports with no call site: {unused}")
    for category in sorted(counts, key=lambda c: CATEGORY_RANK.get(c, 99)):
        print(f"  {category}: {counts[category]}")
    print("  libraries: " + ", ".join(f"{name} ({count})" for name, count in sorted(libraries.items())))

    missing_by_library = {}
    for item in imports:
        if item.category.startswith("MISSING") or item.category == "UNKNOWN":
            entry = missing_by_library.setdefault(item.library, {})
            entry[item.category] = entry.get(item.category, 0) + 1
    if missing_by_library:
        print("  missing by library: " + ", ".join(
            f"{name} ({', '.join(f'{k} {v}' for k, v in sorted(groups.items()))})"
            for name, groups in sorted(missing_by_library.items())))
    missing_used = [item for item in imports if item.category.startswith("MISSING") and item.callsites]
    print(f"  missing with a call site: {len(missing_used)}" +
          (": " + ", ".join(f"{item.symbol or item.nid} ({item.callsites})" for item in missing_used[:24])
           if missing_used else ""))

    if args.missing:
        for item in imports:
            if not item.category.startswith("MISSING") and item.category != "UNKNOWN":
                continue
            extra = f" refs={','.join(item.references)}" if item.references else ""
            print(f"{item.category} calls={item.callsites} {item.scoped} {item.symbol or '?'}{extra}")

    if args.json:
        payload = {
            "boot": str(args.boot),
            "imports": len(imports),
            "libraries": libraries,
            "counts": counts,
            "entries": [item.__dict__ for item in imports],
        }
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        print(f"json: {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
