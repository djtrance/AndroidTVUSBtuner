# TV USB — reproductor Android TV para PT230 y T230

APK Android TV nativa con interfaz controlable por D-pad, escaneo de servicios,
EPG y reproductor Media3/ExoPlayer. Soporta dos receptores, conectados **de a
uno**:

- **PT230 / Siano Rio (`1f4d:a230`)**: ISDB-T, TS completo, servicios 1seg/13seg.
- **T230/RD213 (`0572:c688`)**: DVB-T2, demod Si2168-B40 + tuner Si2158-A20.

El PT230, su protocolo y la recepción/decodificación de TS se validaron en el
dongle real. Para el T230 se incorporaron el puente USB/I2C y las secuencias de
identificación/configuración; queda validar el TS del endpoint `0x82` desde un
Android TV.

El APK graba/reproduce TS en vivo, lista servicios por PAT/PMT, muestra nombres
SDT y actualiza EPG EIT mientras llegan tablas. En T230 ofrece escaneo DVB-T2
de 8 MHz o el plan de 6 MHz seleccionado en pantalla.

```
app/src/main/
├── assets/isdbt_rio.inp          firmware PT230 (local, no versionado)
├── assets/si2168_b40.fw          firmware DVB-T2 Si2168-B40 (local)
├── assets/si2158_a20.fw          firmware tuner Si2158-A20 (local)
├── cpp/siano_proto.{h,cpp}       protocolo/firmware Siano
├── cpp/t230_proto.{h,cpp}        builders USB/I2C y comandos Si2168
├── cpp/jni_bridge.cpp            JNI para ambos protocolos
├── java/.../TvActivity.kt        UI Android TV, escaneo, EPG y Media3
├── java/.../T230Device.kt        T230 DVB-T2 y firmware SiLabs
├── java/.../SianoDevice.kt       PT230, framing 693, stats
└── res/xml/device_filter.xml     VID:PID PT230 y T230
tests/test_proto.cpp              test Siano
tests/test_t230_proto.cpp         test offline del puente DVBsky/SiLabs
```

## Requisitos

- Android Studio (o SDK + NDK + Gradle 8.7+).
- `compileSdk 35`, `minSdk 24`, CMake 3.22.1 (lo instala el SDK).
- Android TV con USB host/OTG. Conecta solo un receptor a la vez.
- T230: la captura de video vía alt0 bulk `0x82` aún requiere validación física
  desde Android; el chip y el protocolo I2C sí se identificaron en el Mac.

## Compilar

```bash
cd driver-skeleton
# local.properties con sdk.dir=/ruta/al/Android/sdk
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Si no tienes `gradlew` en el repo, ábrelo con Android Studio: genera el wrapper
y compila.

## Probar el protocolo sin dongle ni NDK

`tests/test_proto.cpp` compila solo `siano_proto.cpp` con el clang del sistema y
verifica los bytes exactos de cada mensaje y la secuencia completa de firmware
contra el `.inp` real:

```bash
clang++ -std=c++17 -Wall -Wextra -I app/src/main/cpp \
    app/src/main/cpp/siano_proto.cpp tests/test_proto.cpp -o /tmp/test_siano

/tmp/test_siano ../../fw/siano/sms1xxx-rio-isdbt-07.03.58.0.inp
```

Salida esperada (resumen):

```
[+] arranque frio: 382 mensajes
  [ ok ] chunks + SWDOWNLOAD_TRIGGER = total
  [ ok ] DATA_DOWNLOAD con direcciones contiguas
  [ ok ] se envian exactamente length bytes
[+] modo running: 383 mensajes
  [ ok ] running usa la direccion de entrada de payload[20]

TODO OK (0 fallos)
```

## Probar contra el dongle real desde macOS/Linux (sin Android)

`tools/pt230_host.cpp` hace lo mismo que la app pero con libusb, y ya fue
validado: firmware 382 mensajes/0 fallos, chip `0x2270`, TS 100 % sincronizado
a ~15 Mbps.

```bash
clang++ -std=c++17 -O2 -Wall -Wextra \
  -I app/src/main/cpp ../tools/pt230_host.cpp app/src/main/cpp/siano_proto.cpp \
  -I/opt/homebrew/include/libusb-1.0 -L/opt/homebrew/lib \
  -Wl,-rpath,/opt/homebrew/lib -lusb-1.0 -o /tmp/pt230_host

/tmp/pt230_host --fw ../fw/siano/sms1xxx-rio-isdbt-07.03.58.0.inp \
                --scan --dwell 2000
/tmp/pt230_host --no-fw --freq 587142857 --seconds 10 --out /tmp/ch33.ts
```

## Probar en Android TV

1. Instala `app/build/outputs/apk/debug/app-debug.apk` y conecta **un** receptor
   por OTG. Android TV pedirá permiso USB.
2. Pulsa **Abrir receptor**. El tipo se detecta por VID:PID:
   - PT230: carga `isdbt_rio.inp` y arranca ISDB-T en el canal 33.
   - T230 C688: consulta/descarga firmware Si2168-B40 y Si2158-A20; después
     pulsa **Buscar canales**.
3. El escaneo construye una lista con servicios que tienen video, nombre,
   `1seg/13seg` para ISDB-T o `DVB-T2`, PID y EPG. El EIT se vuelve a leer
   continuamente durante la reproducción.
4. Selecciona un servicio con el D-pad para verlo en el reproductor. El PID
   seleccionado se aísla para Media3; la grabación/procesamiento recibe TS del
   receptor y el player usa `ProgressiveMediaSource` + `TsExtractor` multi-PMT.
5. **Detener** cierra el player y el flujo. **Stats**/estado muestra lock, SNR,
   RSSI y bitrate. El reproductor se vuelve a preparar tras una pérdida de lock.

En ISDB-T, frecuencia: `473142857 + (canal - 14) * 6000000` Hz. En DVB-T2, el
escaneo usa 8 MHz/centros de canal UHF 21–69 o el plan de 6 MHz seleccionado.
Si el país usa otra canalización, hace falta ajustar ese plan antes de escanear.

**Estado de validación:** PT230 + TS/player fueron probados en el Mac; los
chips C688 se leyeron directamente por I2C en el Mac y el APK compila. Falta
validar en un Android TV que el endpoint bulk TS `0x82` del T230 entregue video.

## Detalles del protocolo implementados

### PT230 / Siano SMS2270

| Mensaje | ID | Payload |
|---|---|---|
| `SW_RELOAD_START` | 702 | — |
| `DATA_DOWNLOAD` | 660 | `u32 addr` + hasta 240 B |
| `DATA_VALIDITY` | 662 | `{start, length, 0}` |
| `SWDOWNLOAD_TRIGGER` | 664 | `{entry, 6, 0x200, 0, 4}` |
| `SW_RELOAD_EXEC` | 704 | — |
| `INIT_DEVICE` | 578 | `u32 mode` (ISDBT_BDA = 6) |
| `GET_VERSION_EX` | 668 | — |
| `RECEIVE_1SEG_THROUGH_FULLSEG` | 528 | `u32 1` |
| `ISDBT_TUNE` | 776 | `{freq_hz, bw, 12_000_000, 0}`, `src=201` |
| `ADD_PID_FILTER` | 601 | `u32 pid` (**0x2000 = TS completo**), `src=201` |
| `REMOVE_PID_FILTER` | 603 | `u32 pid`, `src=201` |
| `GET_STATISTICS_EX` | 653 | —, `src=201` |

La cabecera es de 8 bytes: `u16 type | u8 src | u8 dst | u16 length | u16 flags`
con `dst = 11 (HIF_TASK)` y `length` incluyendo la cabecera.

### T230 / DVB-T2

`t230_proto.{h,cpp}` implementa y prueba offline el puente I2C del T230:
bulk control OUT `0x01` / IN `0x81`, acceso Si2168 en I2C `0x64`, tuner Si2158
en `0x60`, reset GPIO y control de stream `0x37/0x36`. La app usa los firmwares
Si2168-B40 y Si2158-A20 incluidos en assets. El video TS sale por el bulk IN
`0x82` del alternate setting 0 según `dvbsky.c`.

### Recepción del TS

El chip **no** entrega MPEG-TS "desnudo". Envía mensajes:

```
type = 693 (MSG_SMS_DVBT_BDA_DATA)
[8 B cabecera][payload TS]
```

`SianoDevice.rxLoop()` parsea la cabecera, comprueba `flags & 0x0004`
(`MSG_HDR_FLAG_SPLIT_MSG`) y, si aplica, salta `504 + ((flags >> 8) & 3)`
bytes; luego entrega `len - 8` bytes de payload a `onTs`.

### Secuencia obligatoria

```
firmware (702/660/662/664) → INIT_DEVICE(6) → GET_VERSION_EX(668)
  → RECEIVE_1SEG_THROUGH_FULLSEG(528, 1) → ISDBT_TUNE(776)
  → ADD_PID_FILTER(601, 0x2000)   ← sin esto NO hay TS
  → llegan mensajes 693
```

## Pendiente de validación

- Instalar el APK en un Android TV físico y comprobar player/EPG con OTG.
- **T230**: la capa I2C/firmware y el stream bulk `0x82` están implementados,
  pero falta validar el camino completo en Android TV (el C688 se probó por I2C
  en Mac; no se pudo conectar simultáneamente al PT230).
- La canalización DVB-T2 configurada por defecto es de 8 MHz/UHF; escoger 6 MHz
  en UI cuando corresponda. Otras regiones pueden necesitar otro plan de canales.
- Manejo de desconexión física durante el scan/reproducción: ahora se registra
  el error; falta reenumerar y volver a pedir permiso USB automáticamente.

## Aviso

El firmware `isdbt_rio.inp` es propietario de Siano/Geniatech. Se incluye aquí
porque se extrajo del APK que ya lo distribuía y solo debe usarse con hardware
que legítimamente lo trae. No redistribuyas ni el firmware ni
`libpt27xDrv.so`.
