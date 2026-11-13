#!/usr/bin/env python3
"""
identify_usb.py - Identifica receptores USB de TV (familia Geniatech / PadTV)
a partir de su VID:PID y los mapea a modelo, chip, estandar y driver.

Uso:
    python3 tools/identify_usb.py                 # autodetecta (macOS/Linux/Android)
    python3 tools/identify_usb.py --lsusb         # fuerza lectura de `lsusb`
    python3 tools/identify_usb.py --from-file f   # parsea una salida guardada
    python3 tools/identify_usb.py --json          # salida JSON

Fuentes del mapa: res/xml/device_filter.xml y com/Pad/util/UsbDRV.java del APK
PadTV HD 3.0.0.128, mas strings/dynsym de los .so del split arm64.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import subprocess
import sys

# ---------------------------------------------------------------------------
# Mapa VID:PID -> ficha tecnica
#   chip      : demodulador / SoC identificado en el binario o en fuentes
#   tuner     : tuner cuando es un componente separado
#   std       : estandar(es) de recepcion
#   lib       : modulo que implementa el driver dentro del APK
#   conf      : "confirmado" (evidencia en binario/fuente) | "inferido"
# ---------------------------------------------------------------------------
DB: dict[tuple[int, int], dict] = {
    # ---- ITE IT913x (DVB-T) -------------------------------------------------
    (0x048D, 0x9006): dict(model="PT115", chip="ITE IT9135", std="DVB-T",
                          lib="libPTx3Drv.so (PT913x_*)", conf="inferido"),
    (0x048D, 0x9135): dict(model="PT116", chip="ITE IT9135", std="DVB-T",
                          lib="libPTx3Drv.so (PT913x_*)", conf="confirmado"),
    # ---- ITE IT9175 (ISDB-T) ----------------------------------------------
    (0x048D, 0x9175): dict(model="RD9175 / PT9175", chip="ITE IT9175",
                          std="ISDB-T (1seg/full-seg)", lib="libPTx1Drv.so",
                          conf="confirmado"),
    (0x048D, 0xE275): dict(model="PT275C", chip="ITE IT9175 + tuner externo",
                          std="ISDB-T full-seg", lib="libpt275aDrv.so",
                          conf="confirmado"),
    (0x048D, 0x275B): dict(model="PT275B", chip="ITE IT9175 + tuner externo",
                          std="ISDB-T full-seg", lib="libpt275aDrv.so",
                          conf="inferido"),
    # ---- Silicon Labs Si2168 (DVB-T/T2/C) ---------------------------------
    (0x0572, 0xC688): dict(model="T230 / PT360 / RD213", chip="Silicon Labs Si2168-B40",
                           tuner="Si2158-A20 (confirmado por I2C en el dongle conectado)", std="DVB-T/T2/C",
                          lib="libRD213x_* (dentro de libPadTV_jni.so)",
                          conf="confirmado"),
    (0x0572, 0xC689): dict(model="T230C / RD214", chip="Silicon Labs Si2168-D60",
                          tuner="Si2141-A10", std="DVB-T/T2/C",
                          lib="libRD213x_* (dentro de libPadTV_jni.so)",
                          conf="confirmado"),
    (0x0572, 0xC68A): dict(model="T230C2 / C68A", chip="Silicon Labs Si2168-D60",
                          tuner="Si2141-A10", std="DVB-T/T2/C",
                          lib="libRD213x_* (dentro de libPadTV_jni.so)",
                          conf="confirmado"),
    (0x0572, 0x689A): dict(model="PT360A", chip="Silicon Labs Si2168",
                          tuner="Si2158/Si2148", std="DVB-T/T2/C",
                          lib="libRD213x_* (dentro de libPadTV_jni.so)",
                          conf="inferido"),
    (0x0572, 0xA305): dict(model="PT360A_AUGUST", chip="Silicon Labs Si2168",
                          tuner="Si2158/Si2148", std="DVB-T/T2/C",
                          lib="libRD213x_* (dentro de libPadTV_jni.so)",
                          conf="inferido"),
    (0x0572, 0xA682): dict(model="A682", chip="Silicon Labs Si2168",
                          tuner="Si2158/Si2148", std="DVB-T/T2/C",
                          lib="libRD213x_* (dentro de libPadTV_jni.so)",
                          conf="inferido"),
    # ---- Siano SMS2270 "Rio" (ISDB-T 1seg) --------------------------------
    (0x3275, 0x0080): dict(model="PT230 / PLEX PX-S1UD", chip="Siano SMS2270 (Rio)",
                           std="ISDB-T 1seg", lib="libpt27xDrv.so",
                           conf="confirmado"),
    (0x1F4D, 0xA230): dict(model="PT230 (Geniatech / MyGica)", chip="Siano SMS2270 (Rio)",
                           std="ISDB-T 1seg", lib="libpt27xDrv.so",
                           conf="confirmado en hardware; firmware GET_VERSION_EX=07.03.58.0 RIO"),
    (0x1F4D, 0xA270): dict(model="PT230", chip="Siano SMS2270 (Rio)",
                          std="ISDB-T 1seg", lib="libpt27xDrv.so",
                          conf="confirmado"),
    (0x187F, 0x0600): dict(model="T119", chip="Siano Rio",
                          std="ISDB-T 1seg", lib="libpt27xDrv.so",
                          conf="inferido"),
    (0x187F, 0x0202): dict(model="T118", chip="Siano Nova B",
                          std="DVB-T", lib="libpt27xDrv.so", conf="inferido"),
    # ---- ITE IT9300 + Panasonic MN88553 (ISDB-T) ---------------------------
    (0x048D, 0x9306): dict(model="T980", chip="ITE IT9300 + Panasonic MN88553",
                          std="ISDB-T 6 MHz", lib="libPT930Drv.so",
                          conf="confirmado"),
    (0x048D, 0x930A): dict(model="fci9300", chip="ITE IT9300 + Renesas FC8300",
                          std="ISDB-T 1seg/full-seg",
                          lib="libAppFci8300Tune.so", conf="confirmado"),
    # ---- Sony CXD2856 / CXD2858 -------------------------------------------
    (0x0572, 0x2856): dict(model="PT285", chip="Sony CXD2856",
                          std="ISDB-T / DVB-T", lib="libPTx5Drv.so",
                          conf="inferido"),
    # ---- Sony integrado (PT684) -------------------------------------------
    (0x1F4D, 0x6822): dict(model="PT684", chip="Sony integrado (sony_integ_*)",
                          std="DVB-T2 (PLP)", lib="libPT684Drv.so",
                          conf="inferido"),
    # ---- ATSC (libA681 / libA692 / MXL) -----------------------------------
    (0x1F4D, 0xA681): dict(model="A681", chip="LG? / demod ATSC integrado",
                          std="ATSC 1.0", lib="libPadTV_jni.so (libA681_*)",
                          conf="inferido"),
    (0x1F4D, 0xA692): dict(model="A692", chip="demod ATSC integrado",
                          std="ATSC 1.0", lib="libPadTV_jni.so (libA692_*)",
                          conf="inferido"),
    (0x1F4D, 0xD692): dict(model="d692", chip="demod ATSC integrado",
                          std="ATSC 1.0", lib="libPadTV_jni.so (libA692_*)",
                          conf="inferido"),
    (0x1F4D, 0xE691): dict(model="MXL691", chip="MaxLinear MXL691",
                          std="ATSC 1.0", lib="libPadTV_jni.so",
                          conf="inferido"),
    (0x1F4D, 0xE692): dict(model="MXL692", chip="MaxLinear MXL692",
                          std="ATSC 1.0", lib="libPadTV_jni.so",
                          conf="inferido"),
    (0x1F4D, 0xA693): dict(model="A693", chip="ATSC 3.0 SoC", std="ATSC 3.0",
                          lib="libPadTV_jni.so", conf="inferido"),
    (0x1F4D, 0x0693): dict(model="a693", chip="ATSC 3.0 SoC", std="ATSC 3.0",
                          lib="libPadTV_jni.so", conf="inferido"),
    (0x1F4D, 0xA694): dict(model="A694", chip="ATSC 3.0 SoC", std="ATSC 3.0",
                          lib="libPadTV_jni.so", conf="inferido"),
    (0x1F4D, 0x692A): dict(model="PT692C", chip="demod ATSC", std="ATSC 1.0",
                          lib="libPadTV_jni.so", conf="inferido"),
    (0x1F4D, 0x692F): dict(model="AS2056", chip="demod ATSC", std="ATSC 1.0",
                          lib="libPadTV_jni.so", conf="inferido"),
    (0x1F4D, 0xA115): dict(model="A115", chip="demod ATSC", std="ATSC 1.0",
                          lib="libPadTV_jni.so", conf="inferido"),
    # ---- DTMB --------------------------------------------------------------
    (0x0572, 0xD811): dict(model="PT390 / dtmb", chip="demod DTMB", std="DTMB",
                          lib="libPadTV_jni.so", conf="inferido"),
    (0x0572, 0xD812): dict(model="PT390 / dtmb", chip="demod DTMB", std="DTMB",
                          lib="libPadTV_jni.so", conf="inferido"),
    # ---- Otros Geniatech ---------------------------------------------------
    (0x0572, 0x2880): dict(model="PT280", chip="por confirmar", std="por confirmar",
                          lib="libPadTV_jni.so", conf="inferido"),
    (0x0572, 0x2881): dict(model="PT280A", chip="por confirmar", std="por confirmar",
                          lib="libPadTV_jni.so", conf="inferido"),
    (0x0572, 0x2886): dict(model="T232", chip="por confirmar", std="por confirmar",
                          lib="libPadTV_jni.so", conf="inferido"),
    (0x0572, 0xA682): dict(model="A682", chip="Si2168", std="DVB-T/T2/C",
                          lib="libRD213x_*", conf="inferido"),
}

# Chips "Si..." posibles, para desambiguar la memoria del usuario
SI_CHIPS = {
    "Siano SMS2270 (Rio)": "PT230 / 3275:0080 - ISDB-T 1seg",
    "Silicon Labs Si2168": "T230/PT360/RD213 0572:C688 - DVB-T/T2/C",
    "Silicon Labs Si2158 / Si2148 / Si2141": "tuner del Si2168 (i2c 0x60)",
}


def vendor_name(vid: int) -> str:
    return {
        0x048D: "ITE Tech.",
        0x0572: "Conexant / Geniatech",
        0x1F4D: "Geniatech",
        0x187F: "Siano Mobile Silicon",
        0x3275: "PLEX / Siano",
    }.get(vid, "desconocido")


def lookup(vid: int, pid: int) -> dict:
    info = DB.get((vid, pid))
    if info:
        return info
    # fallback: mismo VID, PID parecido -> sugerencia
    return dict(model="NO MAPEADO", chip="desconocido", std="desconocido",
                lib="desconocido", conf="sin datos")


# ---------------------------------------------------------------------------
# Parsers
# ---------------------------------------------------------------------------
def parse_lsusb(text: str) -> list[dict]:
    out = []
    for line in text.splitlines():
        m = re.search(r"ID\s+([0-9a-fA-F]{4}):([0-9a-fA-F]{4})\s*(.*)", line)
        if m:
            out.append(dict(vid=int(m.group(1), 16), pid=int(m.group(2), 16),
                            product=m.group(3).strip()))
    return out


def parse_macos_json(text: str) -> list[dict]:
    out = []

    def walk(node):
        if isinstance(node, dict):
            vid = node.get("vendor_id")
            pid = node.get("product_id")
            if vid and pid:
                m = re.search(r"0x([0-9a-fA-F]+)", str(vid))
                n = re.search(r"0x([0-9a-fA-F]+)", str(pid))
                if m and n:
                    out.append(dict(vid=int(m.group(1), 16),
                                    pid=int(n.group(1), 16),
                                    product=node.get("_name", "")))
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(json.loads(text))
    return out


def parse_macos_text(text: str) -> list[dict]:
    out = []
    cur = ""
    for line in text.splitlines():
        s = line.strip()
        if s.endswith(":") and not s.lower().startswith(("product id", "vendor id")):
            cur = s[:-1]
        m = re.search(r"Product ID:\s*0x([0-9a-fA-F]+)", s)
        if m:
            pid = int(m.group(1), 16)
            vm = re.search(r"Vendor ID:\s*0x([0-9a-fA-F]+)", text)
            # buscar el Vendor ID mas cercano hacia atras
            out.append(dict(vid=-1, pid=pid, product=cur, _pending=True))
        vm = re.search(r"Vendor ID:\s*0x([0-9a-fA-F]+)", s)
        if vm and out and out[-1].get("_pending"):
            out[-1]["vid"] = int(vm.group(1), 16)
            out[-1].pop("_pending", None)
    return [d for d in out if d["vid"] >= 0]


def parse_ioreg_text(text: str) -> list[dict]:
    """Parsea nodos IOUSBHostDevice de `ioreg -p IOUSB -l -w 0`.

    Algunas versiones de macOS/System Information no imprimen SPUSBDataType
    aunque el dispositivo sí esté en el árbol IORegistry.
    """
    out = []
    current: dict | None = None
    for line in text.splitlines():
        node = re.search(r"\+-o\s+([^<]+)<class IOUSBHostDevice", line)
        if node:
            if current and "vid" in current and "pid" in current:
                out.append(current)
            current = {"product": node.group(1).strip()}
            continue
        if current is None:
            continue
        for key, field in (("idVendor", "vid"), ("idProduct", "pid")):
            m = re.search(rf'"{key}"\s*=\s*(\d+)', line)
            if m:
                current[field] = int(m.group(1), 10)
        for key, field in (("USB Product Name", "product"), ("USB Vendor Name", "manufacturer")):
            m = re.search(rf'"{key}"\s*=\s*"([^"]*)"', line)
            if m:
                current[field] = m.group(1)
        if line.strip() == "}" and "vid" in current and "pid" in current:
            out.append(current)
            current = None
    if current and "vid" in current and "pid" in current:
        out.append(current)
    return out


def parse_sysfs() -> list[dict]:
    out = []
    base = "/sys/bus/usb/devices"
    if not os.path.isdir(base):
        return out
    for name in os.listdir(base):
        d = os.path.join(base, name)
        try:
            vid = int(open(os.path.join(d, "idVendor")).read().strip(), 16)
            pid = int(open(os.path.join(d, "idProduct")).read().strip(), 16)
            prod = open(os.path.join(d, "product")).read().strip()
        except OSError:
            continue
        out.append(dict(vid=vid, pid=pid, product=prod))
    return out


def run(cmd: list[str]) -> str:
    try:
        return subprocess.run(cmd, capture_output=True, text=True,
                              timeout=20).stdout
    except Exception:
        return ""


def collect(force_lsusb: bool, from_file: str | None) -> list[dict]:
    if from_file:
        text = open(from_file, encoding="utf-8", errors="replace").read()
        if "ID " in text and re.search(r"ID\s+[0-9a-fA-F]{4}:", text):
            return parse_lsusb(text)
        return parse_macos_text(text)

    sysname = platform.system()
    if force_lsusb or sysname == "Linux":
        txt = run(["lsusb"])
        devs = parse_lsusb(txt)
        if devs:
            return devs
        return parse_sysfs()
    if sysname == "Darwin":
        raw = run(["system_profiler", "SPUSBDataType", "-json"])
        if raw.strip().startswith("{"):
            devs = parse_macos_json(raw)
            if devs:
                return devs
        devs = parse_macos_text(run(["system_profiler", "SPUSBDataType"]))
        if devs:
            return devs
        return parse_ioreg_text(run(["ioreg", "-p", "IOUSB", "-l", "-w", "0"]))
    # Android / Termux u otros
    txt = run(["lsusb"])
    if txt:
        return parse_lsusb(txt)
    return parse_sysfs()


# ---------------------------------------------------------------------------
def report(devs: list[dict], as_json: bool) -> int:
    rows = []
    for d in devs:
        info = lookup(d["vid"], d["pid"])
        rows.append(dict(vid=f"0x{d['vid']:04X}", pid=f"0x{d['pid']:04X}",
                         usb_product=d.get("product", ""), **info))

    if as_json:
        print(json.dumps(rows, indent=2, ensure_ascii=False))
        return 0

    if not rows:
        print("No se encontro ningun dispositivo USB. "
              "Conecta el dongle y vuelve a ejecutar.")
        print("\nChips 'Si...' posibles en esta familia:")
        for k, v in SI_CHIPS.items():
            print(f"  - {k:38} {v}")
        return 1

    relevant = [r for r in rows if r["model"] != "NO MAPEADO"]
    if relevant:
        rows = relevant

    w = [12, 12, 26, 34, 22, 34, 12]
    hdr = ["VID:PID", "Modelo", "Chip", "Estandar", "Driver (.so)", "Evidencia", "USB"]
    print("  ".join(h.ljust(x) for h, x in zip(hdr, w)))
    print("  ".join("-" * x for x in w))
    for r in rows:
        vals = [f"{r['vid']}:{r['pid']}", r["model"], r["chip"], r["std"],
                r["lib"], r["conf"], r["usb_product"][:12]]
        print("  ".join(str(v).ljust(x) for v, x in zip(vals, w)))

    print("\nComo leerlo:")
    print("  * Si tu dongle dice 'Si...' y el VID:PID es 3275:0080 o 1F4D:A270")
    print("    -> PT230, chip Siano SMS2270 (Rio), ISDB-T 1seg.")
    print("  * Si el VID:PID es 0572:C688/C689/C68A o 0572:689A")
    print("    -> Si2168 (Silicon Labs), DVB-T/T2/C (T230/PT360).")
    print("  * Si el VID:PID es 048D:E275 o 048D:275B")
    print("    -> PT275/PT275C, chip ITE IT9175 (NO es 'Si'), ISDB-T full-seg.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lsusb", action="store_true", help="forzar parseo de lsusb")
    ap.add_argument("--from-file", help="parsear una salida guardada")
    ap.add_argument("--json", action="store_true", help="salida JSON")
    args = ap.parse_args()
    return report(collect(args.lsusb, args.from_file), args.json)


if __name__ == "__main__":
    sys.exit(main())
