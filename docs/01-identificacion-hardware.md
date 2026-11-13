# 01 - Identificación del hardware: ¿cuál es el chip "Si..."?

> Conclusión corta: en esta familia hay **dos** chips que empiezan por "Si" y son
> de fabricantes distintos. El modelo que dijiste (PT230 / PT275) decide cuál:
>
> - **PT230 → chip Siano SMS2270 "Rio"** (Siano empieza por "Si"). Es ISDB-T **1seg**.
> - **PT275 / PT275C → chip ITE IT9175** (no es "Si"). Es ISDB-T **full-seg**.
> - **Si el dongle resulta ser el de DVB-T2 (PT360/PT362/T230) → Silicon Labs Si2168**.
>
> Por eso "Si algo" encaja mejor con el **PT230 (Siano)**. Pero si el tuyo es el de
> DVB-T2, el chip es **Si2168**. Abajo está cómo confirmarlo en 10 segundos.

---

## 1. Los tres candidatos

| Chip | Fabricante | Modelos / VID:PID | Estándar | Driver dentro del APK |
|------|-----------|-------------------|----------|------------------------|
| **SMS2270 "Rio"** | Siano Mobile Silicon | **PT230**, PLEX PX-S1UD<br>`1F4D:A230` (real), `1F4D:A270`, `3275:0080` | ISDB-T **1seg** (6 MHz, 13 segmentos) | `libpt27xDrv.so` (porte de `smsusb` de Linux) |
| **Si2168-B40** | Silicon Labs | **T230 / PT360 / PT362 / RD213**<br>`0572:C688` | DVB-T/T2/C | `libRD213x_*` compilado dentro de `libPadTV_jni.so`; confirmado por I2C en el dongle conectado |
| **Si2168-D60** | Silicon Labs | **T230C / RD214 / C68A**<br>`0572:C689`, `0572:C68A` | DVB-T/T2/C | idem |
| **Si2183-B60** | Silicon Labs | no asociado a un PID en el `device_filter` | ISDB-T + DVB-T2/C/S2 | idem (código presente, uso no confirmado) |
| IT9175 | ITE Tech. | **PT275 / PT275C / PT275B / RD9175**<br>`048D:E275`, `048D:275B`, `048D:9175` | ISDB-T full-seg | `libpt275aDrv.so` / `libPTx1Drv.so` |

Evidencia de que el PT275 **no** es "Si": `libpt275aDrv.so` contiene el núcleo
`it9175_init`, `it9175_set_frontend`, `it9175_read_snr`,
`it9175_read_signal_strength`, `IT9175 proprietary (raw) mode`, etc.

Evidencia de que el PT230 es Siano: `libpt27xDrv.so` incluye
`demod_2270_attach`, los nombres de placa `Siano Rio/Nova/Stellar/Vega`,
`ISDBT 07.03.58.0 RIO` y los mensajes `MSG_SMS_*` del protocolo Siano.

Evidencia del Si2168: el blob `dvb-demod-si2168-b40-01.fw` está embebido
byte a byte en `libPadTV_jni.so` (offset `0x11ae45`), y las funciones
`Si2168_*`, `Si2157_*`, `Si2141_*` están compiladas en la misma librería.

### Validación directa del DVB-T2 conectado al Mac

El segundo dongle conectado aparece como **`0572:c688`**, `bcdDevice=0x0800`,
sin nombre ni serial USB. El descriptor muestra una interfaz vendor-specific
`ff/01/01`; en alt 0 expone bulk OUT `0x01` e IN `0x81`, además de `0x82` y
`0x86`. Alt 1–6 ofrecen endpoint isócrono IN `0x82` para el TS.

No me quedé solo con el VID:PID: `tools/t230_probe.cpp` usó el puente USB/I2C
normal del T230 y consultó los registros de identificación:

| Componente | Respuesta leída | Identificación |
|---|---|---|
| Demodulador, I2C `0x64` | GET_REV: `80 42 44 34 30 ...` | **Silicon Labs Si2168-B40** |
| Tuner, I2C `0x60` detrás de la compuerta del demod | GET_REV: `80 41 3a 32 30 ...`, ROM `0x33` | **Silicon Labs Si2158-A20** |

La prueba reclamó la interfaz y envió la secuencia normal de inicialización/
power-up y GET_REV; **no** descargó firmware, no sintonizó y no inició TS.
Salida reproducible:

```bash
clang++ -std=c++17 -O2 -Wall -Wextra -Werror tools/t230_probe.cpp \
  -I/opt/homebrew/include/libusb-1.0 -L/opt/homebrew/lib \
  -Wl,-rpath,/opt/homebrew/lib -lusb-1.0 -o mac-tuner/build/t230_probe
mac-tuner/build/t230_probe
```

---

## 2. Cómo confirmarlo (aunque el dongle no esté a mano ahora)

### 2.1 Si puedes conectarlo a un Android

Cualquier app de "USB Device Info" (o `adb shell lsusb` con root) te da
`VendorId` y `ProductId`. Con eso:

```bash
python3 tools/identify_usb.py --json
```

En Termux:

```bash
pkg install usbutils
lsusb
python3 tools/identify_usb.py --lsusb
```

### 2.2 Si puedes conectarlo al Mac

```bash
system_profiler SPUSBDataType
python3 tools/identify_usb.py
```

Busca `Vendor ID: 0x....` y `Product ID: 0x....`.

### 2.3 Si no puedes conectarlo

- Mira la **etiqueta del conector o de la caja**: `PT230`, `PT275`, `PT275C`,
  `PT360`, `PT362`, `T230`.
- Mira el **conector**: PT275C y PT362 son USB-C; PT275, PT230 y PT360 son
  micro-USB.
- Mira el **estándar del país**: si lo usabas en Brasil/Perú/Chile/Japón/Filipinas
  es ISDB-T; si es Europa/Australia/África es DVB-T2.
- 1seg vs full-seg: si la app sintonizaba el canal **"1seg"** (calidad baja,
  ~416 kbps, pensada para móviles) es **PT230/Siano**. Si sintonizaba el canal
  **full-seg** (HD, ~15-17 Mbps) es **PT275/IT9175**.
- En Brasil, el 1seg va en el mismo canal de 6 MHz que el full-seg; PadTV muestra
  ambos como servicios separados. Que solo apareciera el 1seg apunta a Siano.

---

## 3. Qué implica cada caso para tu objetivo (driver/APK propio)

### Caso A - PT230, Siano SMS2270 (ISDB-T 1seg)

Es el caso **más fácil de reimplementar**, porque ya existe una implementación
en espacio de usuario independiente del kernel:

- Proyecto: `Khronos31/siano-userland` (GitHub) — `sianod`, `sianoctl`, `siano-ts`.
- Soporta `3275:0080` (PX-S1UD) en Linux, macOS, **Android/Termux** (aarch64/armv7).
- Pasa el FD de USB (`termux-usb -e ... /dev/bus/usb/...`) y entrega **MPEG-TS
  crudo por stdout**.
- **El firmware ya está extraído del APK**: `fw/siano/sms1xxx-rio-isdbt-07.03.58.0.inp`
  (91.048 B, el que descarga `libpt27xDrv.so`). No hace falta pedírselo a
  Geniatech ni buscarlo en foros.

Con eso puedes hacer tu propia APK: USB en Java/Kotlin → `UsbDeviceConnection`
→ FD → binario nativo (o reimplementar el protocolo Siano en C++/NDK) → TS →
MediaCodec/ExoPlayer. El protocolo está documentado en
`drivers/media/common/siano/smscoreapi.h` (IDs `MSG_SMS_*`) y en
`drivers/media/usb/siano/smsusb.c`.

El detalle completo del PT230 (transporte por `ioctl`, tabla de mensajes
verificada, secuencia de arranque y experimento full-seg) está en
[`docs/02-siano-pt230.md`](02-siano-pt230.md), y el esqueleto de APK en
[`driver-skeleton/`](../driver-skeleton/README.md).

### Caso B - PT360/PT362/T230, Silicon Labs Si2168 (DVB-T2)

También es muy reimplementable y es el caso con **mejor soporte open source**:

- Linux mainline: `dvb-usb-dvbsky` + `si2168` + `si2157`/`si2141`.
- I2C: demod `Si2168` en `0x64`, tuner `Si2157/2158` en `0x60`, TS en paralelo,
  `if_port = 1` (confirmado en `dvbsky.c` y en el APK).
- Firmware: `dvb-demod-si2168-b40-01.fw` (ya extraído en `fw/extracted/`) y
  `dvb-tuner-si2158-a20-01.fw`.
- En Android puedes portar el driver como módulo USB en C++/NDK y exponer el TS
  por un socket local, o usar una ROM con el driver en el kernel.

Ventaja frente al 1seg: el full-TS de DVB-T2 permite **multi-PLP**, HEVC/H.265,
EPG, subtítulos y ~15-30 Mbps; es lo que PadTV llama "HD".

### Caso C - PT275/PT275C, ITE IT9175 (ISDB-T full-seg)

Es el caso **más cerrado**: ITE no publica el driver Linux del IT9175 y el
protocolo USB propietario solo está implementado en `libPTx1Drv.so` /
`libpt275aDrv.so`. No hay driver mainline (el `af9035.c` de Linux cubre
IT9135, no IT9175).

Aun así se puede:
- Reutilizar la `.so` del APK (es la vía realista) con el mismo contrato JNI.
- Reimplementar el transporte: el `.so` no hace `ioctl` de USB; todo el I/O pasa
  por la vtable Java (`DrvFactory::Init(void*, PFjPvjS0_E)`), así que se puede
  sustituir la capa Java por otra app.
- Documentar el protocolo interno a partir de `libPT917x_*` (Init/SetPara/
  ReadTS/AddPid/GetQuality) y de los strings `DL_*` / `DRV_*`.
- La recepción full-seg a 6 MHz da ~15-17 Mbps de TS; los PID se filtran por
  hardware (`libPT917x_AddPid`).

---

## 4. Tabla completa VID:PID del APK (para referencia)

| VID:PID | Clase Java | Modelo | Chip / notas |
|---------|-----------|--------|--------------|
| `048D:9006` | PT115 | PT115 | IT9135 (DVB-T), vía `libPTx3Drv.so` |
| `048D:9135` | PT116 | PT116 | IT9135 (DVB-T) |
| `048D:9175` | RD9175 | RD9175 | IT9175, ISDB-T 1seg, `libPTx1Drv.so` |
| `048D:E275` | PT275C | PT275C | IT9175 + tuner, ISDB-T full-seg, `libpt275aDrv.so` |
| `048D:275B` | 275b | PT275B | IT9175, ISDB-T full-seg |
| `048D:9306` | T980 | T980 | IT9300 + Panasonic MN88553, ISDB-T |
| `048D:930A` | fci9300 | fci9300 | IT9300 + Renesas FC8300, ISDB-T |
| `0572:C688` | RD213 | T230 / PT360 | **Si2168-B40 + Si2158-A20; ambos consultados por I2C en el dongle conectado** |
| `0572:C689` | RD214 | T230C | Si2168-D60 + Si2141, DVB-T2 |
| `0572:C68A` | C68A | T230C2 | Si2168-D60 + Si2141, DVB-T2 |
| `0572:689A` | PT360A | PT360A | Si2168 (inferido) |
| `0572:A305` | PT360A_AUGUST | PT360A | Si2168 (inferido) |
| `0572:2856` | PT285 | PT285 | Sony CXD2856, `libPTx5Drv.so` |
| `0572:D811/D812` | dtmb | PT390 | DTMB |
| `0572:2880/2881/2886` | pt280/PT280A/T232 | varios | por confirmar |
| `0572:A682` | A682 | A682 | Si2168 (inferido) |
| `1F4D:A230` | PT230 | PT230 | **Siano SMS2270 Rio, ISDB-T, CONFIRMADO en hardware** (`chip_model=0x2270`, fw `07.03.58.0 RIO`, serial `SMS0000000001`) |
| `1F4D:A270` | PT230 | PT230 | Siano SMS2270 Rio, ISDB-T 1seg (variante) |
| `3275:0080` | PT230 | PLEX PX-S1UD | Siano SMS2270 Rio, ISDB-T 1seg |
| `187F:0600` | T119 | T119 | Siano Rio, ISDB-T |
| `187F:0202` | T118 | T118 | Siano Nova B, DVB-T |
| `1F4D:A681` | A681 | A681 | ATSC 1.0 |
| `1F4D:A692/D692` | A692/d692 | A692 | ATSC 1.0 |
| `1F4D:E691/E692` | MXL691/692 | MXL691/692 | MaxLinear, ATSC |
| `1F4D:692A/692F` | PT692C/AS2056 | varios | ATSC |
| `1F4D:A693/A694/0693` | A693/A694/a693 | varios | ATSC 3.0 |
| `1F4D:6822` | pt684 | PT684 | Sony integrado, DVB-T2 |
| `1F4D:A115` | A115 | A115 | ATSC |

> `conf="inferido"` = el PID está en el `device_filter.xml`, pero el chip no se
> pudo confirmar por strings/símbolos. Los confirmados son los que tienen
> evidencia en el binario o en las fuentes públicas (LinuxTV/datasheets).

---

## 5. Dongle ya identificado y validado

El dongle ISDB-T probado en el Mac respondió como **`1f4d:a230`**, chip
**SMS2270 (`0x2270`)**, firmware **`ISDBT 07.03.58.0 RIO`**. Ya no hace falta
`identify_usb.py`: el protocolo se validó en vivo y se capturó TS real.

El segundo receptor, DVB-T2, se validó por separado como **`0572:c688`** con
demod **Si2168-B40** y tuner **Si2158-A20** (GET_REV por el puente I2C). No se
conectan simultáneamente; la APK permite escoger uno u otro por VID:PID.

| Recurso | Qué contiene |
|---|---|
| `docs/02-siano-pt230.md` | Transporte, protocolo, firmware, escaneo y captura de TS reales (§5 y §9) |
| `tools/pt230_host.cpp` | Driver host macOS/Linux (libusb): firmware, sintonía, scan PAT/PMT con detección de video, métricas y captura TS |
| `mac-tuner/` | App macOS: sintonía, escaneo seleccionable de canales con video, métricas, grabación `.ts` y player embebido |
| `tools/carve_siano_fw.py` | Extrae los `.inp` embebidos de cualquier `.so` de Geniatech |
| `fw/siano/*.inp` | Los tres firmwares del `libpt27xDrv.so` (dos ISDB-T Rio + un DVB-T) |
| `driver-skeleton/` | APK Android TV compilada: Siano live player/EPG y backend T230 DVB-T2 en prueba |
| `driver-skeleton/tests/test_proto.cpp` | Test de escritorio del protocolo (sin NDK) |
| `driver-skeleton/tests/test_t230_proto.cpp` | Test offline del puente DVBsky/Si2168, ASan/UBSan |
| `tools/t230_probe.cpp` | Identifica Si2168-B40 y Si2158-A20 por I2C desde macOS |

Resultados en vivo (canal 33 de Chile, 587.142857 MHz):

- Firmware: 382 mensajes, 0 fallos, 66 ms.
- `GET_VERSION_EX`: `chip_model=0x2270`, `fw_version=0x4006`.
- Escaneo PAT/PMT: diez canales con servicios de video; separa TVN HD/NTV
  (13seg) y TVN Móvil (1seg), con nombres y EPG donde está disponible.
- TS capturado: **100 % de sync, ~15 Mbps**.
- `bw=4` y `bw=8` entregan el mismo TS completo: el 1seg es un programa dentro
  del TS, no un modo del sintonizador.

Comandos:

```bash
# Escaneo con estadísticas reales
/tmp/pt230_host --no-fw --scan --ch-start 14 --ch-end 51 --dwell 2000

# Captura de TS
/tmp/pt230_host --no-fw --freq 587142857 --seconds 10 --out /tmp/ch33.ts
```

Re-extraer el firmware (si actualizas el APK):

```bash
python3 tools/carve_siano_fw.py apk/xapk/libs/lib/arm64-v8a/libpt27xDrv.so \
        -o fw/siano --refs
```

Probar el protocolo sin dongle ni Android SDK:

```bash
clang++ -std=c++17 -Wall -Wextra -I driver-skeleton/app/src/main/cpp \
    driver-skeleton/app/src/main/cpp/siano_proto.cpp \
    driver-skeleton/tests/test_proto.cpp -o /tmp/test_siano
/tmp/test_siano fw/siano/sms1xxx-rio-isdbt-07.03.58.0.inp
```

Debe terminar en `TODO OK (0 fallos)`.
