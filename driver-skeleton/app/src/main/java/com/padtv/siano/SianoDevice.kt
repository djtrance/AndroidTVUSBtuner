package com.padtv.siano

import android.util.Log
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * Secuencia de arranque del PT230 / Siano SMS2270 "Rio".
 *
 * Pasos (ver docs/02-siano-pt230.md):
 *   1. Descarga del firmware .inp: 702/660/662/664/704  (o 664 si arranque frio)
 *   2. INIT_DEVICE (578) con modo ISDBT_BDA = 6
 *   3. GET_VERSION_EX (668)
 *   4. RECEIVE_1SEG_THROUGH_FULLSEG (528) = 1
 *   5. ISDBT_TUNE (776) por canal
 *   6. ADD_PID_FILTER (601) con PID 0x2000 = TS completo (obligatorio:
 *      sin esto el chip no emite nada, aunque el demod este enganchado)
 *   7. El TS llega como mensajes 693 (MSG_SMS_DVBT_BDA_DATA): 8 bytes de
 *      cabecera + payload TS. No hay que buscar 0x47: el framing lo dice.
 */
class SianoDevice(private val transport: UsbTransport) {

    @Volatile
    private var running = false
    private var rxThread: Thread? = null

    /** (buffer, bytes validos) con paquetes MPEG-TS. */
    var onTs: ((ByteArray, Int) -> Unit)? = null

    /** Mensaje de control recibido (msg_type). */
    var onControl: ((type: Int, payload: ByteArray, length: Int) -> Unit)? = null

    /** Linea de log para la UI. */
    var onLog: ((String) -> Unit)? = null

    /** Error de control USB (normalmente desconexión del dongle). */
    var onTransportError: ((String) -> Unit)? = null

    /** Respuestas de control (no-TS) para esperar ACKs. */
    private data class ControlMsg(val type: Int, val payload: ByteArray, val length: Int)

    private val controlQueue = LinkedBlockingQueue<ControlMsg>()

    @Volatile
    private var waitAcks = true

    private fun log(msg: String) {
        Log.d(TAG, msg)
        onLog?.invoke(msg)
    }

    fun start(firmware: ByteArray) {
        require(firmware.size > 12) { "firmware .inp vacio" }
        transport.open()
        log("abierto ${transport.device.deviceName} " +
            "IN=${transport.endpointIn?.address} OUT=${transport.endpointOut?.address}")
        log("protocolo ${Native.protocolVersion()}")

        running = true
        rxThread = Thread({ rxLoop() }, "siano-rx").also { it.start() }

        // 1. firmware
        val fwMessages = Native.firmwareMessages(firmware, false)
        log("descargando firmware: ${fwMessages.size} mensajes, ${firmware.size} bytes")
        var failedAcks = 0
        var sent = 0
        for (m in fwMessages) {
            if (!running) return
            val type = Native.peekType(m)
            val expect = when (type) {
                MSG_DATA_DOWNLOAD -> MSG_DATA_DOWNLOAD_RES
                MSG_DATA_VALIDITY -> MSG_DATA_VALIDITY_RES
                MSG_SWDOWNLOAD_TRIGGER -> MSG_SWDOWNLOAD_TRIGGER_RES
                else -> -1
            }
            val ok = if (expect > 0 && waitAcks) {
                sendWait(m, expect, 400)
            } else {
                send(m, "fw")
                true
            }
            if (!ok) {
                failedAcks++
                if (failedAcks == 1) {
                    waitAcks = false
                    log("sin ACK del firmware; sigo enviando sin esperar")
                }
            }
            if (++sent % 50 == 0) log("fw $sent/${fwMessages.size}")
        }
        if (failedAcks == 0) log("firmware: ACK en todos los bloques")
        Thread.sleep(400)  // el driver original espera 400 ms tras el trigger

        // 2-4. init y modo ISDB-T
        if (!sendWait(Native.buildInit(DEVICE_MODE_ISDBT_BDA), MSG_INIT_DEVICE_RES)) {
            log("INIT_DEVICE sin respuesta")
        }
        if (sendWait(Native.buildSimple(MSG_GET_VERSION_EX, SRC_HOST), MSG_GET_VERSION_EX_RES)) {
            log("version OK")
        }
        sendWait(Native.buildWithWord(MSG_RECEIVE_1SEG_THROUGH_FULLSEG, 1, SRC_HOST),
                 MSG_RECEIVE_1SEG_RES, 800)
    }

    /**
     * Sintoniza un canal ISDB-T (6 MHz) y arranca el flujo de TS.
     *
     * @param freqHz frecuencia central. Mexico/Brasil:
     *               473_142_857 + (canal - 14) * 6_000_000
     * @param bandwidth [BW_ISDBT_1SEG] (4) o [BW_ISDBT_13SEG] (8). En el
     *        hardware probado ambos entregan el mismo TS completo (~15 Mbps):
     *        el parametro describe el ancho de canal, no la capa a decodificar.
     *
     * Tras el tune es imprescindible ADD_PID_FILTER con [PID_FULL_TS], igual
     * que hace smsdvb_start_feed() en Linux. Sin ese mensaje no llega TS.
     */
    fun tune(freqHz: Int, bandwidth: Int = BW_ISDBT_13SEG) {
        sendWait(Native.buildTune(freqHz, bandwidth), MSG_ISDBT_TUNE_RES)
        send(Native.buildPidFilter(true, PID_FULL_TS), "add_pid(0x2000)")
    }

    fun addPid(pid: Int) = send(Native.buildPidFilter(true, pid), "add_pid($pid)")
    fun removePid(pid: Int) = send(Native.buildPidFilter(false, pid), "remove_pid($pid)")
    fun requestStatistics() {
        if (transport.isOpen) send(Native.buildSimple(MSG_GET_STATISTICS_EX, SRC_BDA), "stats")
    }

    private fun send(msg: ByteArray, tag: String) {
        val rc = transport.bulkOut(msg)
        if (rc < 0) {
            val error = "$tag: bulk OUT fallo rc=$rc"
            log(error)
            onTransportError?.invoke(error)
        } else {
            log("$tag: $rc bytes")
        }
    }

    /** Envia y espera una respuesta concreta (como smscore_sendrequest_and_wait). */
    private fun sendWait(msg: ByteArray, expect: Int, timeoutMs: Long = 1500): Boolean {
        controlQueue.clear()
        if (transport.bulkOut(msg) < 0) {
            val error = "bulk OUT falló (esperaba $expect)"
            log("sendWait: $error")
            onTransportError?.invoke(error)
            return false
        }
        val deadline = System.currentTimeMillis() + timeoutMs
        while (running) {
            val left = deadline - System.currentTimeMillis()
            if (left <= 0) break
            val m = controlQueue.poll(left, TimeUnit.MILLISECONDS) ?: break
            if (m.type == expect) return true
        }
        log("sin respuesta $expect (timeout ${timeoutMs}ms)")
        return false
    }

    private fun rxLoop() {
        val buffer = ByteArray(UsbTransport.IN_BUFFER)
        while (running) {
            val n = try {
                transport.bulkIn(buffer)
            } catch (t: Throwable) {
                log("RX excepcion: ${t.message}")
                break
            }
            if (n <= 0) continue

            // Puede venir mas de un mensaje por transferencia: se recorre.
            var off = 0
            while (off + 8 <= n) {
                // Cabecera en el cable:
                //   u16 type | u8 src | u8 dst | u16 len | u16 flags
                val type = le16(buffer, off)
                val len = le16(buffer, off + 4)
                if (len < 8) break

                // Respuesta fragmentada: la cabecera sigue en `off`, pero el
                // payload real esta desplazado response_alignment +
                // ((flags >> 8) & 3). En el Rio = 504.
                val flags = le16(buffer, off + 6)
                var payloadOff = off + 8
                if (flags and FLAG_SPLIT_MSG != 0) {
                    payloadOff = off + RESPONSE_ALIGN + ((flags shr 8) and 3) + 8
                }
                val msgEnd = payloadOff + (len - 8)
                if (msgEnd > n) {
                    log("mensaje truncado type=$type len=$len n=$n")
                    break
                }
                val payloadLen = len - 8

                if (type == MSG_DVBT_BDA_DATA) {
                    // 693 = TS puro: el payload empieza justo tras la cabecera
                    // (no se busca el byte 0x47). Se copia para no exponer el
                    // buffer reutilizable del RX.
                    if (payloadLen > 0) {
                        onTs?.invoke(buffer.copyOfRange(payloadOff, msgEnd), payloadLen)
                    }
                } else {
                    val payload = buffer.copyOfRange(payloadOff, msgEnd)
                    handleControl(type, payload)
                    controlQueue.offer(ControlMsg(type, payload, payloadLen))
                    onControl?.invoke(type, payload, payloadLen)
                    // Los ACK del firmware son 380+ mensajes: no llenar la UI.
                    if (type != MSG_DATA_DOWNLOAD_RES &&
                        type != MSG_DATA_VALIDITY_RES &&
                        type != MSG_SWDOWNLOAD_TRIGGER_RES
                    ) {
                        log("control msg_type=$type len=$len")
                    }
                }
                off = msgEnd
            }
        }
    }

    /** Logs concretos de los mensajes de control mas utiles. */
    private fun handleControl(type: Int, payload: ByteArray) {
        when (type) {
            MSG_GET_VERSION_EX_RES -> {
                if (payload.size < 8) return
                val chip = le32(payload, 0)
                val fw = le32(payload, 4)
                val name = if (payload.size > 14) {
                    val raw = payload.copyOfRange(14, payload.size)
                        .takeWhile { it != 0.toByte() }
                        .toByteArray()
                    String(raw, Charsets.US_ASCII).trim()
                } else {
                    ""
                }
                log("chip_model=0x%x fw_version=0x%x \"%s\"".format(chip, fw, name))
            }
            MSG_GET_STATISTICS_EX_RES -> {
                // struct sms_isdbt_stats con un prefijo de 4 bytes: rf/demod lock.
                if (payload.size >= 0x20) {
                    log("stats rf_lock=${le32(payload, 0x0c)} " +
                        "demod_lock=${le32(payload, 0x10)} " +
                        "SNR=${le32(payload, 0x18)} " +
                        "RSSI=${le32(payload, 0x1c)}")
                }
            }
        }
    }

    private fun le16(b: ByteArray, i: Int): Int =
        (b[i].toInt() and 0xff) or ((b[i + 1].toInt() and 0xff) shl 8)

    private fun le32(b: ByteArray, i: Int): Int =
        (b[i].toInt() and 0xff) or
            ((b[i + 1].toInt() and 0xff) shl 8) or
            ((b[i + 2].toInt() and 0xff) shl 16) or
            ((b[i + 3].toInt() and 0xff) shl 24)

    fun stop() {
        running = false
        rxThread?.join(1500)
        rxThread = null
        transport.close()
        log("cerrado")
    }

    companion object {
        private const val TAG = "SianoDevice"

        // IDs de mensaje (ver siano_proto.h / smscoreapi.h)
        const val MSG_RECEIVE_1SEG_THROUGH_FULLSEG = 528
        const val MSG_RECEIVE_1SEG_RES = 529
        const val MSG_GET_VERSION_EX = 668
        const val MSG_GET_VERSION_EX_RES = 669
        const val MSG_INIT_DEVICE_RES = 579
        const val MSG_GET_STATISTICS_EX = 653
        const val MSG_GET_STATISTICS_EX_RES = 654
        const val MSG_DATA_DOWNLOAD = 660
        const val MSG_DATA_DOWNLOAD_RES = 661
        const val MSG_DATA_VALIDITY = 662
        const val MSG_DATA_VALIDITY_RES = 663
        const val MSG_SWDOWNLOAD_TRIGGER = 664
        const val MSG_SWDOWNLOAD_TRIGGER_RES = 665
        const val MSG_ISDBT_TUNE = 776
        const val MSG_ISDBT_TUNE_RES = 777
        const val MSG_DVBT_BDA_DATA = 693

        /** 0x2000 = todos los PIDs (TS completo). */
        const val PID_FULL_TS = 0x2000

        /** Bit 2 de msg_flags: respuesta partida en dos URBs. */
        private const val FLAG_SPLIT_MSG = 0x0004

        /** Alineacion de las respuestas partidas en el firmware Rio. */
        private const val RESPONSE_ALIGN = 504

        // enum SMS_DEVICE_MODE
        const val DEVICE_MODE_ISDBT_BDA = 6

        // enum sms_bandwidth_mode
        const val BW_ISDBT_1SEG = 4
        const val BW_ISDBT_3SEG = 5
        const val BW_ISDBT_13SEG = 8

        // 0 = host, 201 = DVBT_BDA_CONTROL_MSG_ID
        const val SRC_HOST = 0
        const val SRC_BDA = 201
    }
}
