package com.padtv.siano

import android.util.Log
import java.io.IOException

/** DVB-T/T2/C T230 frontend: Si2168-B40 demod + Si2158 tuner. */
class T230Device(private val transport: T230UsbTransport) {
    data class Stats(
        val rfLock: Boolean,
        val demodLock: Boolean,
        val snrMilliDb: Int,
        val frequencyHz: Int,
    )

    @Volatile private var running = false
    @Volatile private var ready = false
    private var rxThread: Thread? = null
    private var tunerReady = false
    private val i2cMutex = Any()

    var onTs: ((ByteArray, Int) -> Unit)? = null
    var onLog: ((String) -> Unit)? = null
    var onStats: ((Stats) -> Unit)? = null

    private fun log(message: String) {
        Log.d(TAG, message)
        onLog?.invoke(message)
    }

    fun start(demodFirmware: ByteArray, tunerFirmware: ByteArray) {
        require(demodFirmware.size % FW_RECORD_SIZE == 0) { "firmware Si2168 inválido" }
        require(tunerFirmware.size % FW_RECORD_SIZE == 0) { "firmware Si2158 inválido" }
        transport.open()
        log("T230 abierto ${transportDeviceDescription()}")
        performReset()

        command(DEMOD_ADDR, byteArrayOf(0xc0.toByte(), 0x12, 0x00, 0x0c, 0x00, 0x0d, 0x16, 0, 0, 0, 0, 0, 0), 0)
        command(DEMOD_ADDR, byteArrayOf(0xc0.toByte(), 0x06, 0x01, 0x0f, 0, 0x20, 0x20, 0x01), 1)
        val demodRevision = command(DEMOD_ADDR, byteArrayOf(0x02), 13)
            ?: throw IOException("Si2168 GET_REV falló")
        check(demodRevision.size >= 13 && demodRevision[1] == 'B'.code.toByte() &&
            demodRevision[3] == '4'.code.toByte() && demodRevision[4] == '0'.code.toByte()) {
            "demod no es Si2168-B40: ${demodRevision.toHex()}"
        }
        log("demod Si2168-B40 GET_REV=${demodRevision.toHex()}")
        downloadFirmware(DEMOD_ADDR, demodFirmware)
        command(DEMOD_ADDR, byteArrayOf(0x01, 0x01), 1)
        val demodVersion = command(DEMOD_ADDR, byteArrayOf(0x11), 10)
        log("firmware Si2168=${demodVersion?.toHex()}")
        setTsBus(true)

        // Enable the I2C mux and identify/boot the RF tuner behind the demod.
        command(DEMOD_ADDR, byteArrayOf(0xc0.toByte(), 0x0d, 0x01), 0)
        command(TUNER_ADDR, byteArrayOf(0xc0.toByte(), 0x00, 0x0c, 0, 0, 1, 1, 1, 1, 1, 1, 2, 0, 0, 1), 1)
        val tunerRevision = command(TUNER_ADDR, byteArrayOf(0x02), 13)
            ?: throw IOException("Si2158 GET_REV falló")
        check(tunerRevision.size >= 13 && tunerRevision[2].toInt() and 0xff == 58) {
            "tuner no es Si2158: ${tunerRevision.toHex()}"
        }
        log("tuner Si2158-${tunerRevision[1].toInt().toChar()}${tunerRevision[3].toInt().toChar()}${tunerRevision[4].toInt().toChar()} ROM=0x%02x".format(tunerRevision[12].toInt() and 0xff))
        downloadFirmware(TUNER_ADDR, tunerFirmware)
        command(TUNER_ADDR, byteArrayOf(0x01, 0x01), 1)
        val tunerVersion = command(TUNER_ADDR, byteArrayOf(0x11), 10)
        log("firmware Si2158=${tunerVersion?.toHex()}")
        command(TUNER_ADDR, byteArrayOf(0x14, 0, 1, 5, 1, 0), 1)
        command(TUNER_ADDR, byteArrayOf(0x14, 0, 1, 6, 1, 0), 1)
        command(TUNER_ADDR, byteArrayOf(0x14, 0, 1, 7, 1, 0), 1)
        command(DEMOD_ADDR, byteArrayOf(0xc0.toByte(), 0x0d, 0x00), 0)
        tunerReady = true
        ready = true
        running = true
        rxThread = Thread({ rxLoop() }, "t230-rx").also { it.start() }
    }

    /** deliverySystem: 0x70 DVB-T2, 0x20 DVB-T; bandwidthMHz is 6/7/8. */
    fun tune(frequencyHz: Int, bandwidthMHz: Int = 8, deliverySystem: Int = DVB_T2) {
        synchronized(i2cMutex) {
        require(frequencyHz in 42_000_000..870_000_000) { "frecuencia fuera de rango" }
        require(bandwidthMHz in 6..8) { "ancho de banda debe ser 6, 7 u 8 MHz" }
        check(tunerReady) { "T230 aún no está inicializado" }

        command(DEMOD_ADDR, byteArrayOf(0xc0.toByte(), 0x0d, 0x01), 0)
        command(TUNER_ADDR, byteArrayOf(0x14, 0, 3, 7, (0x20 or bandwidthMHz).toByte(), 0), 4)
        command(TUNER_ADDR, byteArrayOf(0x14, 0, 2, 7, 1, 0), 4) // T230: IF port 1
        val tuneTuner = byteArrayOf(
            0x41, 0, 0, 0,
            (frequencyHz and 0xff).toByte(),
            ((frequencyHz shr 8) and 0xff).toByte(),
            ((frequencyHz shr 16) and 0xff).toByte(),
            ((frequencyHz shr 24) and 0xff).toByte(),
        )
        command(TUNER_ADDR, tuneTuner, 1)
        command(TUNER_ADDR, byteArrayOf(0x42, 0), 12) // wait/observe tuner lock
        command(DEMOD_ADDR, byteArrayOf(0xc0.toByte(), 0x0d, 0x00), 0)

        // Linux si2168_set_frontend(), DVB-T2 mode and automatic PLP.
        command(DEMOD_ADDR, byteArrayOf(0x88.toByte(), 2, 2, 2, 2), 5)
        command(DEMOD_ADDR, byteArrayOf(0x89.toByte(), 0x21, 0x06, 0x11, 0x89.toByte(), 0x20), 3)
        command(DEMOD_ADDR, byteArrayOf(0x52, 0xff.toByte(), 0x00), 1)
        command(DEMOD_ADDR, byteArrayOf(0x51, 0x03), 12)
        command(DEMOD_ADDR, byteArrayOf(0x12, 0x08, 0x04), 3)
        command(DEMOD_ADDR, byteArrayOf(0x14, 0, 0x0c, 0x10, 0x12, 0), 4)
        command(DEMOD_ADDR, byteArrayOf(0x14, 0, 0x06, 0x10, 0x24, 0), 4)
        command(DEMOD_ADDR, byteArrayOf(0x14, 0, 0x07, 0x10, 0, 0x24), 4)
        command(DEMOD_ADDR, byteArrayOf(0x14, 0, 0x0a, 0x10, (deliverySystem or bandwidthMHz).toByte(), 0), 4)
        command(DEMOD_ADDR, byteArrayOf(0x14, 0, 0x0f, 0x10, 0x10, 0), 4)
        // T230 attach config sets ts_clock_inv=1 in dvbsky.c, so don't OR 0x10.
        command(DEMOD_ADDR, byteArrayOf(0x14, 0, 0x09, 0x10, 0xe3.toByte(), 0x08), 4)
        command(DEMOD_ADDR, byteArrayOf(0x14, 0, 0x08, 0x10, 0xd7.toByte(), 0x05), 4)
        command(DEMOD_ADDR, byteArrayOf(0x14, 0, 0x01, 0x12, 0, 0), 4)
        command(DEMOD_ADDR, byteArrayOf(0x14, 0, 0x01, 0x03, 0x0c, 0), 4)
        command(DEMOD_ADDR, byteArrayOf(0x85.toByte()), 1)
        setTsBus(true)

        // dvbsky_stream_ctrl(): start data path on USB bulk endpoint 0x82.
        transport.control(byteArrayOf(0x37, 0, 0), 0)
        Thread.sleep(20)
        transport.control(byteArrayOf(0x36, 3, 0), 0)
        log("tune DVB-${if (deliverySystem == DVB_T2) "T2" else "T"} $frequencyHz Hz / ${bandwidthMHz}MHz")
        }
    }

    fun requestStatistics() {
        if (!ready) return
        val response = command(DEMOD_ADDR, byteArrayOf(0x50, 0x01), 14) ?: return
        if (response.size >= 4) {
            val lockBits = (response[2].toInt() ushr 1) and 3
            onStats?.invoke(Stats(
                rfLock = lockBits >= 1,
                demodLock = lockBits == 3,
                snrMilliDb = (response[3].toInt() and 0xff) * 250,
                frequencyHz = 0,
            ))
        }
    }

    private fun performReset() {
        val steps = listOf(
            Triple(0x04, 0x01, 20L),
            Triple(0x83, 0x00, 0L),
            Triple(0xc0, 0x01, 100L),
            Triple(0x83, 0x01, 0L),
            Triple(0xc0, 0x00, 50L),
        )
        steps.forEach { (port, value, delay) ->
            transport.control(byteArrayOf(0x0e, port.toByte(), value.toByte()), 1)
            if (delay > 0) Thread.sleep(delay)
        }
    }

    private fun setTsBus(enabled: Boolean) {
        // ts_mode=parallel (0x06), ts_clock_gapped=true; both flags are in args[4].
        val mode = if (enabled) 0x56 else 0x50
        command(DEMOD_ADDR, byteArrayOf(0x14, 0, 1, 0x10, mode.toByte(), 0), 4)
    }

    private fun downloadFirmware(address: Int, firmware: ByteArray) {
        if (firmware.size % FW_RECORD_SIZE != 0) throw IOException("firmware de tamaño inválido")
        var off = 0
        var records = 0
        while (off < firmware.size) {
            val length = firmware[off].toInt() and 0xff
            if (length !in 1..16 || off + 1 + length > firmware.size) {
                throw IOException("registro de firmware inválido en $off")
            }
            val cmd = firmware.copyOfRange(off + 1, off + 1 + length)
            command(address, cmd, 1)
            off += FW_RECORD_SIZE
            records++
        }
        log("firmware I2C cargado: $records comandos")
    }

    private fun command(address: Int, bytes: ByteArray, responseLength: Int): ByteArray? {
        synchronized(i2cMutex) {
            val write = Native.buildT230I2cWrite(address, bytes)
            val ack = transport.control(write, 1)
            if (ack.isEmpty()) throw IOException("I2C write sin ACK del puente")
            if (responseLength == 0) return ByteArray(0)

            val readFrame = Native.buildT230I2cRead(address, responseLength)
            repeat(50) {
                val raw = transport.control(readFrame, responseLength + 1)
                val response = Native.extractT230I2cData(raw, responseLength)
                    ?: throw IOException("respuesta I2C truncada")
                if (response.isNotEmpty() && response[0].toInt() and 0x80 != 0) {
                    if (response[0].toInt() and 0x40 != 0) {
                        throw IOException("Si2168/Si2158 reportó comando inválido: ${response.toHex()}")
                    }
                    return response
                }
                Thread.sleep(3)
            }
            throw IOException("timeout esperando CTS I2C en 0x%02x".format(address))
        }
    }

    private fun rxLoop() {
        val buffer = ByteArray(TS_BUFFER_SIZE)
        while (running) {
            val count = try { transport.readTs(buffer) } catch (e: Throwable) {
                if (running) log("T230 TS read error: ${e.message}")
                break
            }
            if (count > 0) onTs?.invoke(buffer.copyOf(count), count)
        }
    }

    fun stop() {
        running = false
        ready = false
        try { transport.control(byteArrayOf(0x37, 0, 0), 0) } catch (_: Throwable) { }
        rxThread?.join(1200)
        rxThread = null
        transport.close()
        tunerReady = false
        log("T230 detenido")
    }

    fun stopStreaming() {
        try { transport.control(byteArrayOf(0x37, 0, 0), 0) }
        catch (e: Throwable) { log("T230 stream stop: ${e.message}") }
    }

    private fun transportDeviceDescription(): String =
        "${transportDeviceVendor()}:%04x".format(T230UsbTransport.PID)

    private fun transportDeviceVendor(): String = "%04x".format(T230UsbTransport.VID)

    private fun ByteArray.toHex(): String = joinToString(" ") { "%02x".format(it.toInt() and 0xff) }

    companion object {
        private const val TAG = "T230Device"
        private const val DEMOD_ADDR = 0x64
        private const val TUNER_ADDR = 0x60
        private const val FW_RECORD_SIZE = 17
        private const val TS_BUFFER_SIZE = 0x4000
        const val DVB_T = 0x20
        const val DVB_T2 = 0x70
    }
}
