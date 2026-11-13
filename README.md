# TV USB — Android TV USB tuner

Proyecto para receptores USB de TV con una app Android TV y una herramienta de
prueba para macOS.

## Receptores

| Receptor | Identificación | Estado |
|---|---|---|
| Geniatech/MyGica PT230 | `1f4d:a230`, Siano SMS2270 Rio, ISDB-T | Protocolo y TS validados en vivo; app Android implementada. |
| MyGica T230/RD213 | `0572:c688`, Si2168-B40 + Si2158-A20 | Chips confirmados por I2C en el dongle. Backend Android y escaneo están integrados; falta validar el TS desde Android TV. |

Solo se usa **un receptor a la vez**. La app detecta cuál está conectado.

## Android TV

El proyecto está en [`driver-skeleton/`](driver-skeleton/README.md). Incluye
launcher/banner Android TV, USB host, reproductor Media3, escaneo de servicios,
EPG, estadísticas de señal y las rutas de PT230 y T230.

Abre `driver-skeleton/` en Android Studio y ejecuta `assembleDebug`, o usa una
instalación local de Gradle con SDK/NDK configurados:

```bash
cd driver-skeleton
gradle :app:assembleDebug
```

Instalar en un Android TV conectado por ADB:

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

### Firmware (no incluido en Git)

Por ser firmware de proveedor, los blobs se excluyen del repositorio. Antes de
ejecutar la app, coloca los archivos que puedas usar legítimamente en:

```text
driver-skeleton/app/src/main/assets/isdbt_rio.inp
driver-skeleton/app/src/main/assets/si2168_b40.fw
driver-skeleton/app/src/main/assets/si2158_a20.fw
```

El Android build empaqueta estos assets si están presentes. El PT230 necesita
`isdbt_rio.inp`; el T230 necesita los dos `.fw` de SiLabs. El firmware no forma
parte de los commits de este repositorio.

## Sintonizador macOS

La app nativa y sus instrucciones están en [`mac-tuner/`](mac-tuner/README.md).
Requiere Homebrew `libusb` y `ffmpeg`, además del firmware ISDB-T local. El
script acepta `ISDBT_FIRMWARE=/ruta/al/firmware.inp`.

## Investigación/documentación

- [`docs/01-identificacion-hardware.md`](docs/01-identificacion-hardware.md): VID:PID y chips comprobados.
- [`docs/02-siano-pt230.md`](docs/02-siano-pt230.md): protocolo Siano, firmware, TS y pruebas.
- `tools/pt230_host.cpp`: captura/escaneo macOS/Linux del PT230.
- `tools/t230_probe.cpp`: lectura de identidad Si2168/Si2158 por el puente USB/I2C.

No se incluyen el APK original, bibliotecas propietarias extraídas ni firmware
del proveedor. Consulta los avisos de licencia de cada componente antes de
redistribuir software o firmware compilado.
