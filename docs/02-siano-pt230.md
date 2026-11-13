# 02 - PT230: Siano SMS2270 "Rio" (ISDB-T, micro-USB)

> **Identificación cerrada.** 1seg + micro-USB + chip "Si..." = **PT230**, chip
> **Siano SMS2270 (familia SMS1140 "Rio")**. No es un PT275: el PT275/PT275C es
> ISDB-T *full-seg*, lleva **ITE IT9175** (`it9175_init` en `libpt275aDrv.so`) y
> el PT275C es USB-C.
>
> Etiquetas: **[E]** verificado en binario/fuente · **[I]** inferencia pendiente
> de confirmar con el dongle.

---

## 1. Ficha del dispositivo

| Campo | Valor |
|---|---|
| Modelo | PT230 (Geniatech / MyGica PadTV) |
| VID:PID | **`1F4D:A230` (confirmado en hardware real)**, `1F4D:A270` (variante de la misma familia), `3275:0080` (PLEX PX-S1UD) **[E]** |
| Descriptor USB | bcdUSB 2.0, 1 configuración, interfaz 0 `ff/ff/ff`, bulk IN `0x81` / OUT `0x02`, maxpkt 512 **[E]** |
| Strings | `Geniatech` / `USB Device` / `SMS0000000001` **[E]** |
| Chip | Siano SMS2270 (`chip_model=0x2270`), board "Rio" **[E]** |
| Firmware | `ISDBT 07.03.58.0 RIO`, `fw_version=0x4006` **[E]** |
| Estándar | ISDB-T (Brasil/Perú/Chile/Argentina/México, 6 MHz) **[E]** |
| USB | micro-USB 2.0, interfaz 0, bulk **[E]** |
| Driver en el APK | `libpt27xDrv.so` **[E]** |
| Clase JNI | `PT230` → `libpt27x_*` **[E]** |

> **Nota sobre `a230` vs `a270`:** PadTV HD solo declara `a270` en su
> `device_filter.xml`, pero el `a230` real funciona igual porque el `.so` no
> filtra por PID (el APK aplica su propia tabla en Java). Añadimos `a230` al
> esqueleto.

PadTV HD declara el `a270` y el `3275:0080` como
`com.geniatech.dvb.devices.PT230` en
`analysis/padtv_apktool/res/xml/device_filter.xml`: **[E]**

```xml
<usb-device product-id="41584" vendor-id="8013">  <!-- 1f4d:a270 -->
    <device-class name="com.geniatech.dvb.devices.PT230" />
</usb-device>
<usb-device product-id="128" vendor-id="12917">   <!-- 3275:0080 -->
    <device-class name="com.geniatech.dvb.devices.PT230" />
</usb-device>
```

El `a230` real no está en ese XML; lo detecta la tabla Java `UsbDRV`
(`vendorIDs = {1165, 1394, 8013, 6271}`) y el `.so` no filtra por PID. **[E]**

`libPadTV_jni.so` declara `DT_NEEDED` sobre `libpt27xDrv.so` y llama a sus
símbolos **sin `dlopen`**: `libpt27x_Init`, `SetPara`, `GetQuality`,
`GetStrength`, `IsLock`, `i2c_read`, `i2c_write`, `Close`. **[E]**

---

## 2. Arquitectura real dentro de PadTV HD

```
Java  com.Pad.util.UsbDRV
  UsbManager.openDevice() -> UsbDeviceConnection     (fd real del kernel)
  claimInterface(0)
        │  fd + JNI callbacks
        ▼
libPadTV_jni.so
  PT230::Init(ctx, cb)  ──►  libpt27x_Init(ctx, cb)
      ctx  = objeto con tabla de funciones (init, get_fd, send, preload)
      cb   = TS_SMS_CallBack  →  DVBDemux::InjectSMSBuffer()
        ▼
libpt27xDrv.so
  ioctl(fd, USBDEVFS_BULK, &bulktransfer)   ← transporte propio, sin libusb
        ▼
USB bulk OUT 0x02  /  USB bulk IN 0x81
```

### 2.1 `PT230::Init` (JNI)

`PT230` es un *thunk* de 24 bytes en `0x67a54`:

```asm
mov  x8, x1                      ; x8 = callback Java
ldr  x1, [x19, #0xaf8]           ; x1 = &TS_SMS_CallBack
str  x8, [x0, #0x10]             ; guarda el callback
mov  x0, x8
b    libpt27x_Init               ; libpt27x_Init(ctx, cb)
```

`TS_SMS_CallBack` (`0x62b14`) es la función que recibe los buffers de TS y los
inyecta en `DVBDemux::InjectSMSBuffer(buf, len)`. **[E]**

### 2.2 `libpt27x_Init` (`0x3248`)

| Dirección | Qué hace |
|---|---|
| `0x627b0` | guarda el callback de TS |
| `0x627d0` | guarda el `ctx` (tabla de funciones) |
| `0x627c0` | `ldr w0,[ctx,#0x10]` → **fd de USB**, lo guarda aquí |
| `0x627c8` | contexto interno creado por `fcn.0x4498` |
| `0x6607f0` | `pthread_create(..., 0x3150, NULL)` → hilo de recepción |

El log que lo confirma es `%s dev_handle=%ld Line=%d` con el valor devuelto por
`ctx[0x10]`: **es el file descriptor**, no un product id (corrección respecto a
la versión anterior de este documento). **[E]**

Del `ctx` se usan además `[0x18]` (handle opaco) y `[0x60]` (envío bulk OUT:
`ctx->send(handle, buf, len)`), y `[0x68]` (preload opcional durante la
descarga de firmware). **[E]**

---

## 3. Transporte USB (corregido)

`libpt27xDrv.so` **no** usa `libusb` ni `UsbRequest`: hace `ioctl` directo sobre
el fd que le pasa Java. **[E]**

### 3.1 Escritura (`fcn.0x3004` / `0x3074` / `0x30e4`)

```asm
ldr   w0, [x8, #0x7c0]     ; fd
mov   w1, #0x5502          ; USBDEVFS_BULK
movk  w1, #0xc018, lsl 16  ; 0xC0185502
mov   w8, #0x86a0
movk  w8, #1, lsl 16       ; timeout = 100000
str   w8, [sp, #8]
str   x1, [sp, #0x10]      ; data
bl    ioctl
```

El argumento es `struct usbdevfs_bulktransfer`:

```c
struct usbdevfs_bulktransfer {
    unsigned int ep;       /* endpoint, p.ej. 0x02 OUT / 0x81 IN */
    unsigned int len;
    unsigned int timeout;  /* ms */
    void        *data;
};
```

### 3.2 Endpoints

| Sentido | Endpoint | Evidencia |
|---|---|---|
| OUT | `0x02` | constante en `.rodata` y llamadas con `w2=2`; **confirmado en descriptor real** |
| IN | `0x81` | constante `{0x81, 0x4000}` en `.rodata:0x5280`; **confirmado en descriptor real** |

`{0x81, 0x4000}` = endpoint IN + **buffer de 16 KiB**. El hilo `0x3150` hace
`memset(16 KiB)` → `bulk IN` → `fcn.0x44e0(buffer)` → callback de TS. **[E]**

> Nota: el stack de Linux usa 10 URBs en paralelo (`MAX_URBS 10`); este driver
> usa **una lectura bloqueante de 16 KiB por iteración**. Para tu propia app
> conviene encolar varios `UsbRequest` para no perder paquetes de TS.

### 3.3 `libpt27x_ReadTS` (`0x321c`)

Es un **stub que devuelve 0**: el TS no se lee por polling, llega por el
callback del hilo de recepción. `PT230::ReadTS` también es un stub. **[E]**

---

## 4. Protocolo Siano

### 4.1 Cabecera (8 bytes, little-endian)

```c
struct sms_msg_hdr {
    u16 msg_type;
    u8  msg_src_id;   /* 0 = host, 201 = DVBT_BDA_CONTROL_MSG_ID */
    u8  msg_dst_id;   /* 11 = HIF_TASK */
    u16 msg_length;   /* incluye la cabecera */
    u16 msg_flags;
};
```

`SMS_INIT_MSG` en el SDK pone `src=0`, `dst=HIF_TASK (11)` y `flags=0`. **[E]**

### 4.2 Mensajes que el driver **construye** (verificado instrucción a instrucción)

| ID | Constante | Dónde | Payload |
|---|---|---|---|
| 516 | `SET_MAX_TX_MSG_LEN` | `0x37dc` | `u32 max_len` |
| 528 | `RECEIVE_1SEG_THROUGH_FULLSEG` | `0x3834` | `u32 1` (siempre) |
| 578 | `INIT_DEVICE` | `0x3b54`, `0x3cd0` | `u32` × 1 |
| 601 | `ADD_PID_FILTER` | `0x4204` | `u32 pid` (0x2000 = TS completo) **[E]** |
| 603 | `REMOVE_PID_FILTER` | `0x42cc` | pid/idx |
| 653 | `GET_STATISTICS_EX` | `0x43a0`, `0x4d60` | — |
| 660 | `DATA_DOWNLOAD` | `0x3f30` | `u32 addr` + bloque |
| 662 | `DATA_VALIDITY` | `0x4010` | addr, len, checksum |
| 664 | `SWDOWNLOAD_TRIGGER` | `0x40dc` | addr, prio, stack, 0, task |
| 668 | `GET_VERSION_EX` | `0x3c0c` | `u32 0` |
| 702 | `SW_RELOAD_START` | `0x3e38` | — |
| 704 | `SW_RELOAD_EXEC` | `0x4058` | — |
| 776 | `ISDBT_TUNE` | `0x4c5c` | ver §5 |
| 794 | `NEW_CRYSTAL` | `0x380c` | `u32` |

El resto de IDs que aparecen en `.text` (504, 510, 512, 529, 562, 579, 602,
604, 616, 654, 688, 703, 705, 777...) son **comparaciones de respuesta** en la
máquina de estados, no mensajes salientes. **[E]**

### 4.3 Secuencia de arranque observada

1. `SW_RELOAD_START (702)` ← solo si ya había firmware corriendo.
2. `DATA_DOWNLOAD (660)` en bloques → `DATA_VALIDITY (662)` → `SWDOWNLOAD_TRIGGER (664)` → `SW_RELOAD_EXEC (704)`.
3. Espera ~400 ms.
4. `INIT_DEVICE (578)`.
5. `GET_VERSION_EX (668)` → modelo/revisión de firmware.
6. `SET_MAX_TX_MSG_LEN (516)`, `NEW_CRYSTAL (794)`.
7. `RECEIVE_1SEG_THROUGH_FULLSEG (528)` con **1**.
8. `ISDBT_TUNE (776)` al cambiar de canal.
9. `ADD_PID_FILTER (601)` con `pid = 0x2000` (**obligatorio para que salga TS**).
   El driver construye en `0x4204` una cabecera BDA (src=201) + `u32 pid`,
   total 12 bytes. **[E]**
10. TS por bulk IN `0x81` como mensajes **`693 = MSG_SMS_DVBT_BDA_DATA`**:
    `[8 B cabecera][payload TS]`. No es TS "desnudo" y no hay que buscar
    `0x47`; el payload empieza en el byte 8 del mensaje. **[E]**
    En la práctica **todos** los mensajes de TS llegan con
    `flags & 0x0004` (split): el payload real está en
    `cabecera + 504 + ((flags >> 8) & 3) + 8`. Medido: 1730 mensajes split en
    4 s, TS 100 % sincronizado. **[E]**
11. `GET_STATISTICS_EX (653)` → `654` (288 B) para lock/SNR/RSSI. **[E]**

---

## 5. Tuning ISDB-T: el detalle importante

`libpt27x_SetPara(fep)` (`0x33d0`) registra `fep->frequency` y
`fep->bandwidth`, pero delega el tuning en `ctx->tune(fep)` (`[ctx+0x28]`).
Ese camino construye `MSG_SMS_ISDBT_TUNE_REQ (776)` en `0x4c40` con este
layout de 24 bytes: **[E]**

```
offset 0   : cabecera {msg_type=776, src=201 (BDA), dst=11, len=24, flags=0}
offset 8   : u32 frequency        (= fep->frequency)
offset 0xc : u32 8                (constante)
offset 0x10: u32 12 000 000       (constante, 0x00B71B00)
offset 0x14: u32 0
```

Y el log asociado es literalmente:

```
received fep->frequency = %d, msg.Data[1] = %d
```

El `8` coincide con `BW_ISDBT_13SEG` del `enum sms_bandwidth_mode`
(`1SEG=4`, `3SEG=5`, `13SEG=8`).

### 5.1 Resultado real del experimento 4 vs 8 **[E]**

Probado en un canal mexicano real (587.142857 MHz, canal 33) con el mismo
firmware y el mismo `ADD_PID 0x2000`:

| `Data[1]` | TS capturado | Bitrate | Sync |
|---|---|---|---|
| `8` (13seg) | 11 065 680 B en 6 s | **14.75 Mbps** | **100 %** (58 860/58 860) |
| `4` (1seg)  | 19 758 800 B en 10 s | **15.81 Mbps** | 100 % |

Conclusión: **el parámetro no limita la capa decodificada**. Ambos entregan el
mismo TS completo de 13 segmentos (mismos PIDs, misma PAT). La diferencia de
bitrate es solo la ventana de medida. El 1seg no es un flujo aparte: es un
**programa dentro del TS** (capa A) que se selecciona en el demux por PID, no
cambiando el ancho de banda del tune.

### 5.2 Estadísticas ISDB-T (`653` → `654`) **[E]**

`654` devuelve 288 bytes = **prefijo de 4 bytes + `struct sms_isdbt_stats`**.
Campos verificados (offset ya con el prefijo):

| Offset | Campo |
|---|---|
| `+0x0c` | `is_rf_locked` |
| `+0x10` | `is_demod_locked` |
| `+0x14` | `is_external_lna_on` |
| `+0x18` | `SNR` (dB) |
| `+0x1c` | `RSSI` (dBm) |
| `+0x20` | `in_band_pwr` (dBm) |
| `+0x24` | `carrier_offset` |
| `+0x28` | `frequency` (Hz, coincide con el tune) |
| `+0x2c` | `bandwidth` (6 → 6 MHz) |
| `+0x30` | `transmission_mode` (3 → 8K) |
| `+0x34` | `modem_state` (1 = enganchado, 2 = sin señal) |

Ejemplo real sin antena: `rf_lock=1`, `demod_lock=0`, `SNR=0`,
`RSSI=-46 dBm`, `in_band=-93 dBm`. Con señal: `demod_lock=1`,
`SNR=14..29 dB`, `in_band=-56..-80 dBm`. **[E]**

> `INTERFACE_LOCK_IND (805)` es el lock de la **interfaz USB**, no de RF. Los
> indicadores válidos son `modem_state`/`demod_lock` o `827`/`828`.

### 5.3 Escaneo real de canales (México, 473.142857 MHz = ch14) **[E]**

Banda ISDB-T mexicana: `f = 473142857 + (canal - 14) * 6000000`.

| Canal | Frecuencia | `demod_lock` | SNR | `in_band` |
|---|---|---|---|---|
| 24 | 533.142857 MHz | sí | 21 dB | -62 dBm |
| 26 | 545.142857 MHz | sí | 24 dB | -68 dBm |
| 27 | 551.142857 MHz | sí | 23 dB | -64 dBm |
| 28 | 557.142857 MHz | sí | 16 dB | -72 dBm |
| 29 | 563.142857 MHz | sí | 0 dB reportado | -64 dBm |
| 30 | 569.142857 MHz | sí | 26 dB | -66 dBm |
| 33 | 587.142857 MHz | sí | **29 dB** | -56 dBm |
| 35 | 599.142857 MHz | sí | 25 dB | -64 dBm |
| 36 | 605.142857 MHz | sí | 24 dB | -66 dBm |
| 38 | 617.142857 MHz | sí | 16 dB | -61 dBm |
| 47 | 671.142857 MHz | sí | 14 dB | -80 dBm |
| 50 | 689.142857 MHz | sí | 15 dB | -74 dBm |

Los canales 14–23, 25, 31–32, 34, 37, 39–46, 48–49 y 51 dieron
`demod_lock=0` (sin emisión o demasiado débiles).

> El `demod_lock` aparece en <1 s, pero el **primer TS tarda ~1–2 s** tras el
> tune. Para detectar TS en el escaneo usa `--dwell 2000` o más. **[E]**

El escáner actualizado guarda una muestra del TS y comprueba `PAT → PMT →
stream_type` de video (H.264, MPEG-2, HEVC, etc.), además de `SDT` (nombre/tipo
de servicio) y `EIT` (evento actual/siguiente). Distingue "hay señal" de "hay
un programa con video" y lista cada servicio por separado. El descriptor de
servicio `0xc0` se presenta como **1seg**; los demás servicios de TV, como
**13seg**. Al seleccionar un resultado el reproductor filtra ese `program_id`,
en lugar de reproducir siempre el primer servicio del multiplex.

Prueba real del escaneo en ch33: `rf=1`, `demod=1`, `SNR=28 dB`; servicios
`TVN HD` (13seg, PID `0x0066`), `NTV` (13seg, PID `0x00ca`) y `TVN Movil`
(1seg, PID `0x0386`). La EIT entregó títulos como “PAMPA ILUSIÓN (AVANCE)” y
“BOCA SUCIA”.

Durante reproducción, el host consulta lock cada 2.5 s sin consumir paquetes
TS. La app detecta ausencia de TS y reabre el item de AVPlayer al volver la
señal; también reintenta si AVPlayer marca el item como fallido. **[E]**

Barrido completo 14–51 en este Mac: **10 canales con video** (`24`, `26`, `27`,
`28`, `29`, `30`, `33`, `36`, `47`, `50`). Ch35 y ch38 sí tenían lock/TS, pero
no se añadieron a la lista porque no se encontró una pista de video en PAT/PMT.

---

## 6. Firmware: ya está extraído

`libpt27xDrv.so` **sí** lleva el firmware embebido. La versión anterior de este
documento decía que no se podía localizar; ya está resuelto con prueba de
código.

### 6.1 Prueba

`fcn.0x3d44` es `smscore_load_firmware_from_file` (imprime su propio nombre,
string en `0x4e8f3`). Su prólogo:

```asm
0x3d68  adrp x0, #0x4e000
0x3d70  add  x0, x0, #0x8f3        ; "smscore_load_firmware_from_file"
0x3d6c  mov  w20, #0x639c
0x3d74  movk w20, #1, lsl #16      ; w20 = 0x1639c  (length)
0x3d7c  add  x0, x20, #0xc         ; malloc(0x163a8)
0x3d8c  adrp x1, #0x1e000
0x3d98  add  x1, x1, #0x4a8        ; src = 0x1e4a8
0x3d90  mov  w2, #0x63a8
0x3d94  movk w2, #1, lsl 16        ; w2 = 0x163a8
0x3da8  bl   memcpy
```

Es decir: `memcpy(buf, 0x1e4a8, 0x163a8)` y luego la secuencia `702/660/662/664/704`
que ya vimos. **El blob está en `.rodata` en `0x1e4a8` y mide 91.048 bytes.**

### 6.2 Los tres firmwares del `.so`

Cabecera `struct sms_firmware { u32 check_sum; u32 length; u32 start_address; u8 payload[]; }`.
Los tres tienen `start_address = 0x00040260` (dirección de carga en el chip) y
están pegados uno detrás de otro dentro de `.rodata`. **[E]**

| # | Offset | Total | `length` | `check_sum` | Contenido | ¿Se usa? |
|---|---|---|---|---|---|---|
| A | `0x56a8` | 101.888 | 101.876 | `0xd1` | `DVB 02.0.47.3` | no referenciado |
| B | `0x1e4a8` | **91.048** | **91.036** | `0xa5` | **`ISDBT 07.03.58.0 RIO`** | **sí** (`0x3d8c`) |
| C | `0x34850` | 96.972 | 96.960 | `0x1a` | **`ISDBT 07.05.20 RIO`** | no referenciado |

### 6.3 Extracción reproducible

```bash
python3 tools/carve_siano_fw.py apk/xapk/libs/lib/arm64-v8a/libpt27xDrv.so \
        -o fw/siano --refs
```

Salida (resumida):

```
[1] sms1xxx-rio-isdbt-07.03.58.0.inp
    start=0x40260 checksum=0xa5 length=0x1639c (91036)
    sha256=777c9ba2ce44db9e0a90f7207167e9095cc0223be2b2af599f645f1c73d76b14
    SI (referenciado por codigo) en 0x3d8c

[2] sms1xxx-rio-isdbt-07.05.20.inp
    start=0x40260 checksum=0x1a length=0x17ac0 (96960)
    sha256=8005f84a6ade3acd09c7442c411aa7212a88584858a1e748a7d1d6ca38c56c34
    no referenciado
```

### 6.4 Lo que esto habilita

- **No necesitas pedirle el firmware a Geniatech.** Está en
  `fw/siano/sms1xxx-rio-isdbt-07.03.58.0.inp` y es suficiente para
  `Khronos31/siano-userland` o para un driver propio.
- **Hay una segunda versión, 07.05.20**, más nueva y ya probada (§6.5).
- Los `.fw` públicos que descargamos antes (`sms1xxx-*-dvbt`, `-isdbt`) **no**
  coinciden byte a byte: son de otras boards. El de Rio no estaba publicado.

### 6.5 Prueba en vivo del firmware `07.05.20` **[E]**

Cargado en el dongle real con el camino *running* (`702` → bloques → `704`):

| Dato | Resultado |
|---|---|
| Mensajes de descarga | **407**, 0 fallos, 95 ms |
| `GET_VERSION_EX` | `ISDBT 07.05.20 RIO`, `chip_model=0x2270`, `fw_version=0x4006` |
| `ADD_PID 0x2000` | ahora **sí** responde `602` |
| TS en canal 33 | 12 521 364 B en 8 s (**~12.5 Mbps**), **100 % sync** |

Conclusión: el 07.05.20 arranca, sintoniza y entrega TS válido igual que el
07.03.58.0. **Pero** después de que el host deja de leer, el endpoint de
control OUT empieza a dar timeout; el chip sigue enviando TS pero ya no acepta
`INIT_DEVICE`/`TUNE` hasta reenchufar el USB. No se encontró un mensaje de
"stop" en el driver.

Para el esqueleto se mantiene **07.03.58.0**, que sí permite reabrir y
re-inicializar sin desenchufar. Si quieres usar 07.05.20 en producción, primero
hay que descubrir cómo detener el flujo limpiamente (probablemente un
`REMOVE_PID_FILTER (603)` o un reset del chip por I2C/vendor request).

---

## 7. Qué se puede "explotar" de la recepción

### 7.1 Lo que entrega el 1seg

Un canal ISDB-T de 6 MHz tiene 13 segmentos. El 1seg ocupa **1 segmento**
(capa A, "partial reception") y transporta:

- H.264/AVC de baja resolución, ~300-420 kbps en total.
- Audio AAC (HE-AAC) mono/estéreo.
- Subtítulos, EPG (EIT), datos BML y alertas EWS.

### 7.2 ¿Full-seg? **Sí, confirmado en hardware** **[E]**

El SMS2270 entrega el **TS completo de 13 segmentos** (~15 Mbps) por USB. El
`bw=4` no lo restringe (ver §5.1). Por tanto:

- No hace falta cambiar de firmware para ver full-seg: el 07.03.58.0 ya lo da.
- El 1seg viaja como **programa dentro del TS** (capa A); se elige en el demux.
- El límite práctico es el **ancho de banda USB** y el demux, no el chip.
- Queda por probar el firmware `07.05.20` (§6), que podría corregir demodulación
  en canales débiles, pero no es necesario para full-seg.

### 7.3 Lo que sí se puede exprimir desde el primer día

- **TS crudo** por USB: grabación sin recodificar, demux propio, EPG,
  subtítulos, teletexto/BML.
- **Métricas**: `GET_STATISTICS_EX (653)` → C/N, BER, fuerza, lock.
- **Escaneo fino** de potencia/calidad por frecuencia para orientar antena.
- **Network tuner**: servir el TS por UDP/HTTP a otro equipo.
- **No hay IQ**: el chip entrega TS demodulado; para SDR puro hace falta otro
  hardware.

### 7.4 Límites legales

Reutilizar `libpt27xDrv.so` de Geniatech en una app propia es técnicamente
posible, pero es código propietario: no lo redistribuyas. Reimplementar el
protocolo (opción B) es limpio; el firmware `.inp` sigue siendo propietario de
Siano, así que úsalo solo con hardware que ya lo trae.

---

## 8. Camino para tu propia APK/driver

### Opción A - Reutilizar `libpt27xDrv.so`

Contrato mínimo:

```java
UsbDeviceConnection c = usbManager.openDevice(dev);
c.claimInterface(dev.getInterface(0), true);
int fd = c.getFileDescriptor();
// ctx debe exponer: [0x00] init, [0x10] get_fd, [0x18] handle, [0x60] send
nativeInit(ctx, tsCallback);
```

El problema es que `ctx` es una tabla de punteros a funciones C, no una clase
Java: tendrías que crear ese `struct` en C++ dentro de tu JNI y puentear los
`send` a `UsbDeviceConnection.bulkTransfer`. Es factible, pero el ABI no está
documentado y cambia entre versiones del `.so`.

### Opción B - Reimplementar el protocolo (recomendada)

Base: **`Khronos31/siano-userland`**, que ya soporta `3275:0080` en
Linux/macOS/Android/Termux y escribe MPEG-TS por stdout. Sustituye
`termux-usb` por `UsbDeviceConnection` + NDK y reutiliza
`fw/siano/sms1xxx-rio-isdbt-07.03.58.0.inp`.

En este repo tienes el esqueleto en `driver-skeleton/`:

```
driver-skeleton/
├── app/src/main/java/.../UsbTransport.kt    USB host + fd + bulk
├── app/src/main/java/.../SianoDevice.kt     secuencia de arranque
├── app/src/main/cpp/siano_proto.{h,cpp}     cabeceras y mensajes
├── app/src/main/cpp/jni_bridge.cpp          puente fd ↔ C++
└── app/src/main/res/xml/device_filter.xml   1f4d:a230 / a270 / 3275:0080
```

### Opción C - Driver en el kernel

Solo si controlas la ROM: `smsusb` + `smsdvb` mainline soportan `3275:0080`
(board Rio). En Android TV, TIF/Live Channels puede consumir `/dev/dvb/*`.

---

## 9. Validación en vivo con `tools/pt230_host.cpp` **[E]**

Herramienta de línea de comandos (macOS/Linux + libusb) que habla el protocolo
directamente, sin Android ni el driver propietario. Reutiliza
`driver-skeleton/app/src/main/cpp/siano_proto.cpp`.

Compilar en macOS:

```bash
clang++ -std=c++17 -O2 -Wall -Wextra \
  -I driver-skeleton/app/src/main/cpp \
  tools/pt230_host.cpp driver-skeleton/app/src/main/cpp/siano_proto.cpp \
  -I/opt/homebrew/include/libusb-1.0 -L/opt/homebrew/lib \
  -Wl,-rpath,/opt/homebrew/lib -lusb-1.0 -o /tmp/pt230_host
```

Uso:

```bash
# Descarga firmware + init + version + tune + captura TS
/tmp/pt230_host --fw fw/siano/sms1xxx-rio-isdbt-07.03.58.0.inp \
                --freq 587142857 --bw 8 --seconds 10 --out /tmp/ch33.ts

# Si ya hay otro firmware cargado en RAM, usa el camino de recarga:
/tmp/pt230_host --running --fw fw/siano/sms1xxx-rio-isdbt-07.05.20.inp \
                --freq 587142857 --bw 8 --seconds 8 --out /tmp/ch33-v0720.ts

# Escaneo PAT/PMT: lista canales que contienen pista de video
/tmp/pt230_host --no-fw --scan --ch-start 14 --ch-end 51 --dwell 2000

# Solo estadísticas del canal actual
/tmp/pt230_host --no-fw --stats --freq 587142857 --seconds 3

# Elegir PID (0x2000 = todo el TS; por defecto)
/tmp/pt230_host --no-fw --freq 587142857 --pid 0x2000
```

Resultados obtenidos:

| Paso | Resultado |
|---|---|
| Firmware | 382 mensajes, **0 fallos**, 66 ms |
| `INIT_DEVICE` | `579` OK |
| `GET_VERSION_EX` | chip `0x2270`, fw `0x4006`, `ISDBT 07.03.58.0 RIO` |
| `RECEIVE_1SEG` | `529` OK |
| `ISDBT_TUNE` | `777` OK |
| `ADD_PID 0x2000` | TS inmediato (el `602` puede no llegar o llegar tarde) |
| Framing TS | 1730 mensajes `693` con split flag en 4 s, **100 % sync** |
| TS ch33 | 100 % sync, ~15 Mbps, H.264 |

PIDs observados en el TS del canal 33: `0x0066` y `0x00CA` (~7 Mbps cada uno),
más `0x0385`–`0x0387`, `0x0065`/`0x00C9` (tablas/PMT) y `0x1FFF` (relleno).

---

## 10. Pendientes

1. ~~Confirmar VID:PID y descriptor real de endpoints del dongle.~~ Hecho:
   `1f4d:a230`, bulk `0x81`/`0x02`, maxpkt 512.
2. ~~Verificar si `776 Data[1]=8` realmente sintoniza 13 segmentos.~~ Hecho:
   4 y 8 entregan el mismo TS completo; medido ~15 Mbps.
3. ~~Probar el firmware `07.05.20` frente a `07.03.58.0`.~~ Hecho (§6.5):
   arranca y entrega TS, pero deja el OUT bloqueado al cerrar el host.
4. Encontrar el mensaje de "stop"/reset para poder usar `07.05.20` sin
   reenchufar (candidato: `REMOVE_PID_FILTER (603)`).
5. Implementar el `ctx` de la opción A o completar la opción B (el esqueleto ya
   funciona a nivel de protocolo).
6. ~~Medir el TS real: PIDs, bitrate, PAT/PMT.~~ Hecho (§9).
7. Validar el esqueleto Android en un dispositivo con OTG (el host tool prueba
   que el protocolo es correcto; falta la app).
8. Comparar 07.03.58.0 vs 07.05.20 en un canal débil (SNR < 15 dB).

---

## 11. Referencias

- `reference/smsusb.c`, `reference/smscoreapi.c/h`, `reference/smsdvb.c`
  (Linux mainline).
- `reference/si2168.c`, `reference/dvbsky.c` (el otro dongle, DVB-T2).
- `fw/siano/sms1xxx-rio-isdbt-07.03.58.0.inp` (extraído, en uso).
- `fw/siano/sms1xxx-rio-isdbt-07.05.20.inp` (extraído, probado en §6.5).
- `fw/siano/*.fw` (firmwares públicos de otras boards, para comparar).
- `tools/carve_siano_fw.py` (extractor reproducible).
- `tools/pt230_host.cpp` (driver de host libusb, validado en vivo).
- `mac-tuner/` (app gráfica nativa para macOS; canal, métricas, grabación TS y
  reproductor en vivo embebido con AVPlayer + HLS local).
- `tools/identify_usb.py` (identificación del dongle).
- `docs/01-identificacion-hardware.md` (tabla VID:PID completa).
