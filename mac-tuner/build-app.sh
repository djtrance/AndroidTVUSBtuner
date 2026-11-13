#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/mac-tuner/build/PT230 Tuner.app"
CONTENTS="$APP/Contents"
MACOS="$CONTENTS/MacOS"
RESOURCES="$CONTENTS/Resources"

LIBUSB_INCLUDE="${LIBUSB_INCLUDE:-/opt/homebrew/include/libusb-1.0}"
LIBUSB_LIB="${LIBUSB_LIB:-/opt/homebrew/lib}"
FFMPEG_BIN="${FFMPEG_BIN:-$(command -v ffmpeg || true)}"
ISDBT_FIRMWARE="${ISDBT_FIRMWARE:-$ROOT/fw/siano/sms1xxx-rio-isdbt-07.03.58.0.inp}"

if [[ ! -f "$LIBUSB_INCLUDE/libusb.h" || ! -f "$LIBUSB_LIB/libusb-1.0.dylib" ]]; then
  echo "No encuentro libusb. Instálalo con: brew install libusb" >&2
  exit 1
fi
if [[ -z "$FFMPEG_BIN" || ! -x "$FFMPEG_BIN" ]]; then
  echo "No encuentro ffmpeg. Instálalo con: brew install ffmpeg" >&2
  exit 1
fi
if [[ ! -f "$ISDBT_FIRMWARE" ]]; then
  echo "No encuentro el firmware ISDB-T (no se versiona en el repo)." >&2
  echo "Cópialo del firmware autorizado del dongle o indica ISDBT_FIRMWARE=/ruta/al/archivo.inp" >&2
  exit 1
fi

rm -rf "$APP"
mkdir -p "$MACOS" "$RESOURCES"

clang++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -I "$ROOT/driver-skeleton/app/src/main/cpp" \
  "$ROOT/tools/pt230_host.cpp" \
  "$ROOT/driver-skeleton/app/src/main/cpp/siano_proto.cpp" \
  -I"$LIBUSB_INCLUDE" -L"$LIBUSB_LIB" \
  -Wl,-rpath,"$LIBUSB_LIB" -lusb-1.0 \
  -o "$MACOS/pt230_host"

swiftc -parse-as-library -swift-version 5 -O \
  -framework SwiftUI -framework AppKit \
  -framework AVKit -framework AVFoundation -framework Network \
  "$ROOT/mac-tuner/PT230Tuner.swift" \
  "$ROOT/mac-tuner/LocalHLSHTTPServer.swift" \
  -o "$MACOS/PT230Tuner"

cp "$FFMPEG_BIN" "$MACOS/ffmpeg"
cp "$ISDBT_FIRMWARE" "$RESOURCES/isdbt_rio.inp"

cat > "$CONTENTS/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>PT230Tuner</string>
  <key>CFBundleIdentifier</key><string>com.padtv.pt230tuner</string>
  <key>CFBundleName</key><string>PT230 Tuner</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

echo "App creada: $APP"
