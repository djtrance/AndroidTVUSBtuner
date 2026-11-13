#!/usr/bin/env python3
"""
carve_siano_fw.py - extrae los firmwares Siano embebidos en un .so de Android.

Formato (familia SMS1xxx "family2", ver `struct sms_firmware` en smscoreapi.h):

    struct sms_firmware {
        u32 check_sum;       // 0x6c, 0xa5, 0xd1... (distinto por build)
        u32 length;          // bytes de payload
        u32 start_address;   // 0x00040260 en todos los .inp observados
        u8  payload[length]; // ARM del chip
    };

Ojo: el orden es check_sum, length, start_address. El "magic" 0x00040260 es
en realidad la direccion de carga, no una firma.

El .so de Geniatech guarda los .inp enteros (cabecera incluida) dentro de
.rodata y llama a `smscore_load_firmware_from_file`, que hace:

    malloc(length + 12);
    memcpy(buf, <ptr en .rodata>, length + 12);
    smscore_load_firmware_family2(buf, length + 12);

Uso:
    python3 tools/carve_siano_fw.py apk/xapk/libs/lib/arm64-v8a/libpt27xDrv.so -o fw/siano
    python3 tools/carve_siano_fw.py <lib.so> --refs      # muestra que blob carga el codigo

La deteccion de referencias resuelve pares `adrp xN, #pagina` + `add xM, xN, #off`
en .text, que es como el compilador materializa punteros a .rodata. Solo el blob
referenciado por `smscore_load_firmware_from_file` se descarga al dongle.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import math
import os
import re
import struct
import sys

MAGIC_START = 0x00040260  # start_address comun a todos los .inp de esta familia
MIN_LEN = 4096
MAX_LEN = 512 * 1024
MAX_START = 0x01000000
MIN_ENTROPY = 6.0


def entropy(data: bytes) -> float:
    if not data:
        return 0.0
    counts = collections.Counter(data)
    n = len(data)
    return -sum((v / n) * math.log2(v / n) for v in counts.values())


def printable_ratio(data: bytes) -> float:
    if not data:
        return 0.0
    good = sum(1 for b in data if 32 <= b < 127 or b in (9, 10, 13))
    return good / len(data)


def load_elf(path: str):
    """Devuelve (bytes, {nombre: (addr, off, size)}) o (bytes, {}) si no hay pyelftools."""
    data = open(path, "rb").read()
    sections = {}
    try:
        from elftools.elf.elffile import ELFFile  # type: ignore

        with open(path, "rb") as fh:
            elf = ELFFile(fh)
            for sec in elf.iter_sections():
                sections[sec.name] = (
                    sec["sh_addr"],
                    sec["sh_offset"],
                    sec["sh_size"],
                )
    except Exception as exc:  # pragma: no cover - pyelftools opcional
        print(f"[!] sin pyelftools ({exc}); se escanea el archivo completo", file=sys.stderr)
    return data, sections


def find_blobs(data: bytes, sections=None, loose: bool = False):
    """Localiza candidatos por la cabecera {check_sum, length, start_address}.

    Por defecto exige start_address == 0x00040260, que es la direccion de carga
    que comparten todos los .inp Siano observados (incluidos los publicos). Es
    el discriminador que evita los cientos de falsos positivos que produce
    escanear .rodata solo por "length plausible + entropia alta".

    Con `loose=True` acepta cualquier start_address < 16 MB y exige ademas una
    firma textual de firmware Siano ("Siano-MSG") y checksum pequeno.
    """
    blobs = []
    seen_ends = set()
    for off in range(0, len(data) - 12, 4):
        check_sum, length, start = struct.unpack_from("<III", data, off)
        if not (MIN_LEN <= length <= MAX_LEN):
            continue
        if length % 4:
            continue
        total = length + 12
        if off + total > len(data):
            continue
        end = off + total
        if end in seen_ends:
            continue
        payload = data[off + 12 : end]
        if not loose:
            if start != MAGIC_START:
                continue
        else:
            if start > MAX_START or check_sum > 0xFFFF:
                continue
            if b"Siano-MSG" not in payload[: 0x40000]:
                continue
        ent = entropy(payload)
        if ent < MIN_ENTROPY:
            continue
        if printable_ratio(payload) > 0.70:
            continue
        seen_ends.add(end)
        blobs.append(
            {
                "offset": off,
                "check_sum": check_sum,
                "length": length,
                "start": start,
                "total": total,
                "end": end,
                "entropy": ent,
                "printable": printable_ratio(payload),
                "blob": data[off:end],
            }
        )
    blobs.sort(key=lambda b: b["offset"])
    return blobs


def blob_strings(blob: bytes, limit: int = 8):
    """Busca cadenas utiles para identificar el firmware (version/board/tareas)."""
    pats = [
        rb"ISDBT [0-9][ -~]{3,40}",
        rb"DVB[HT]? [0-9][ -~]{3,40}",
        rb"ATSC[ -~]{3,40}",
        rb"TDMB[ -~]{3,40}",
        rb"ISDBT_[A-Z_]{3,40}_TASK",
        rb"Siano[- ][A-Za-z]{2,20}",
    ]
    found, seen = [], set()
    for pat in pats:
        for m in re.finditer(pat, blob):
            text = m.group(0).decode("latin1").strip()
            if text in seen:
                continue
            seen.add(text)
            found.append((m.start(), text))
            if len(found) >= limit:
                return sorted(found)
    return sorted(found)


def section_of(sections, off: int):
    for name, (addr, soff, size) in sections.items():
        if soff <= off < soff + size:
            return name, off - soff + addr
    return "?", off


def resolve_refs(data: bytes, sections, targets):
    """Busca en .text pares adrp+add que apunten a las direcciones dadas."""
    try:
        import capstone  # type: ignore
    except Exception:
        return {}

    text = sections.get(".text")
    if not text:
        return {}
    addr, off, size = text
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    insns = list(md.disasm(data[off : off + size], addr))
    hits = {}
    for i, ins in enumerate(insns):
        if ins.mnemonic != "adrp":
            continue
        m = re.match(r"(\w+), #(0x[0-9a-f]+)", ins.op_str)
        if not m:
            continue
        reg, page = m.group(1), int(m.group(2), 0)
        for nxt in insns[i + 1 : i + 10]:
            if nxt.mnemonic in ("b", "bl", "br", "blr", "ret"):
                break
            mm = re.match(r"(\w+), (\w+), #(0x[0-9a-f]+)", nxt.op_str)
            if nxt.mnemonic == "add" and mm and mm.group(2) == reg:
                value = page + int(mm.group(3), 0)
                if value in targets:
                    hits.setdefault(value, []).append(ins.address)
                break
    return hits


def guess_name(blob: bytes, start: int) -> str:
    txt = " ".join(t for _, t in blob_strings(blob, 12))
    m = re.search(r"ISDBT ([0-9.]+) RIO", txt)
    if m:
        return f"sms1xxx-rio-isdbt-{m.group(1)}.inp"
    m = re.search(r"DVB ([0-9.]+)", txt)
    if m:
        return f"sms1xxx-dvbt-{m.group(1)}.inp"
    m = re.search(r"ATSC-M/H|TDMB", txt)
    if m:
        return f"sms1xxx-{'atscmh' if 'ATSC' in m.group(0) else 'tdmb'}-{start:04x}.inp"
    return f"sms1xxx-unknown-{start:05x}.inp"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("so", help="libreria .so a analizar")
    ap.add_argument("-o", "--out", default="fw/siano", help="directorio de salida (default: fw/siano)")
    ap.add_argument("--refs", action="store_true", help="resolver referencias adrp+add en .text")
    ap.add_argument("--loose", action="store_true", help="aceptar start_address distinto de 0x40260")
    ap.add_argument("--dry-run", action="store_true", help="no escribir archivos")
    args = ap.parse_args()

    data, sections = load_elf(args.so)
    blobs = find_blobs(data, sections, loose=args.loose)
    if not blobs:
        print("[-] no se encontro ningun firmware con cabecera sms_firmware")
        return 1

    referenced = {}
    if args.refs and sections:
        vaddrs = {section_of(sections, b["offset"])[1] for b in blobs}
        referenced = resolve_refs(data, sections, vaddrs)

    print(f"[+] {args.so}: {len(blobs)} firmware(s) embebido(s)")
    os.makedirs(args.out, exist_ok=True)
    manifest = []
    for idx, b in enumerate(blobs):
        sec, vaddr = section_of(sections, b["offset"])
        name = guess_name(b["blob"], b["start"])
        base, ext = os.path.splitext(name)
        if any(m["name"] == name for m in manifest):
            name = f"{base}-{idx}{ext}"
        refs = referenced.get(b["offset"], [])
        refs = referenced.get(vaddr, []) or refs
        used = "SI (referenciado por codigo)" if refs else "no referenciado"
        print(
            f"\n  [{idx}] {name}\n"
            f"      seccion={sec} vaddr=0x{vaddr:x} off=0x{b['offset']:x}..0x{b['end']:x}\n"
            f"      start=0x{b['start']:x} checksum=0x{b['check_sum']:x} "
            f"length=0x{b['length']:x} ({b['length']})\n"
            f"      entropy={b['entropy']:.2f} printable={b['printable']*100:.1f}%\n"
            f"      sha256={hashlib.sha256(b['blob']).hexdigest()}\n"
            f"      {used}" + (f" en {', '.join(hex(x) for x in refs)}" if refs else "")
        )
        for off, text in blob_strings(b["blob"]):
            print(f"        +0x{off:06x}  {text}")
        if not args.dry_run:
            path = os.path.join(args.out, name)
            with open(path, "wb") as fh:
                fh.write(b["blob"])
            print(f"      -> {path}")
        manifest.append(
            {
                "name": name,
                "offset": b["offset"],
                "start": b["start"],
                "length": b["length"],
                "used": bool(refs),
            }
        )

    used = [m for m in manifest if m["used"]]
    if used:
        print(f"\n[=] firmware que descarga este .so: {used[0]['name']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
