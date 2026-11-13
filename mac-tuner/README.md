# PT230 Tuner para macOS

Pequeña app nativa para probar el PT230/Siano Rio conectado directamente al
Mac. Permite sintonizar canales ISDB-T, ver lock/SNR/RSSI y bitrate en vivo, y
guardar el MPEG-TS en un archivo `.ts`. La interfaz usa el controlador host
libusb ya validado (`tools/pt230_host.cpp`).

## Requisitos

- Mac Apple Silicon (el build script usa el Homebrew habitual en `/opt/homebrew`).
- Xcode Command Line Tools / Swift.
- libusb y FFmpeg: `brew install libusb ffmpeg`.
- Firmware ISDB-T que el usuario pueda usar legalmente; no se versiona en este repo.
- Dongle PT230 conectado por USB. Para la primera prueba tras una desconexión,
  usa la versión habitual (`07.03.58.0`).

Esta app controla el **PT230/Siano `1f4d:a230`**. El DVB-T2 **`0572:c688`
(Si2168-B40 + Si2158-A20)** ya está identificado y probado por I2C, pero aún no
está implementado como fuente de video en esta app.

## Construir y abrir

Desde la raíz del proyecto:

```bash
brew install libusb ffmpeg
ISDBT_FIRMWARE=/ruta/al/sms1xxx-rio-isdbt-07.03.58.0.inp ./mac-tuner/build-app.sh
open "mac-tuner/build/PT230 Tuner.app"
```

La app consulta la versión del dongle antes de cada sintonía: si detecta el
firmware Rio activo lo reutiliza, y si encuentra el receptor frío carga el
firmware incluido. Así puede abrirse de nuevo sin confundir arranque frío con
recarga de firmware.
Canal 33 (587.142857 MHz) es un canal que se verificó con señal en este Mac.
El barrido real detectó video en los canales 24, 26, 27, 28, 29, 30, 33, 36,
47 y 50.

## Uso

1. Conecta el PT230 directamente al Mac (si quedó en estado extraño tras
   experimentar con `07.05.20`, desenchúfalo y vuelve a conectarlo).
2. Escribe el canal ISDB-T (14–51) y pulsa **Sintonizar**.
3. La app muestra RF lock, demod lock, SNR, RSSI, bitrate y bytes recibidos.
4. **Escanear canales** recorre 14–51, comprueba lock/PAT/PMT y lista cada
   servicio de video detectado, incluidos **1seg** y **13seg**, con nombre del
   servicio, PID y EPG cuando la señal lo incluye. **Ver EPG** abre la guía en
   una ventana grande. Pulsa una fila para sintonizar ese servicio (no solo el
   primer programa del multiplex).
5. **Grabar TS…** permite escoger la ruta de salida. Al cambiar de canal se
   detiene la captura actual y se inicia la nueva en el archivo seleccionado.
6. **Abrir TS…** reproduce dentro de la misma ventana una captura ya existente.
7. **Detener** cierra el stream, cancela el escaneo o para la reproducción.

Frecuencia usada: `473142857 + (canal - 14) * 6000000` Hz. El control 1seg/13seg
se conserva para experimentar; en las pruebas ambos valores entregaron el TS
completo.

El reproductor integrado recibe el TS en vivo (o una captura seleccionada con
**Abrir TS…**), extrae el servicio seleccionado y prepara HLS para AVPlayer.
Para mejorar compatibilidad, deinterlaza y normaliza el video a H.264 con
VideoToolbox, y convierte el audio AAC-LATM a AAC. La app consulta lock
periódicamente y, si se interrumpe el TS, espera el retorno de datos y reinicia
AVPlayer. La métrica **Video** muestra la resolución detectada o avisa si hay TS
pero no se decodifican cuadros. El `.ts` grabado conserva el transporte original.
Para inspeccionar una captura externamente:

```bash
ffprobe -hide_banner captura.ts
ffplay captura.ts
```

El firmware incluido es propietario de Siano/Geniatech y debe usarse solo con
el hardware que legítimamente lo trae.
