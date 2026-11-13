package com.padtv.siano

import android.content.Context
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbManager
import java.io.IOException

/** Bulk control/TS transport for the DVBsky-compatible T230/RD213 bridge. */
class T230UsbTransport(private val context: Context, private val device: UsbDevice) {
    private var connection: UsbDeviceConnection? = null
    private var usbInterface: android.hardware.usb.UsbInterface? = null
    private var endpointControlIn: UsbEndpoint? = null
    private var endpointControlOut: UsbEndpoint? = null
    private var endpointTsIn: UsbEndpoint? = null

    val isOpen: Boolean get() = connection != null

    fun open() {
        if (isOpen) return
        require(device.vendorId == VID && device.productId == PID) {
            "T230UsbTransport solo acepta 0572:c688"
        }
        val intf = device.getInterface(0)
        for (i in 0 until intf.endpointCount) {
            val ep = intf.getEndpoint(i)
            if (ep.type != UsbConstants.USB_ENDPOINT_XFER_BULK) continue
            when (ep.address) {
                EP_CONTROL_IN -> endpointControlIn = ep
                EP_CONTROL_OUT -> endpointControlOut = ep
                EP_TS_IN -> endpointTsIn = ep
            }
        }
        checkNotNull(endpointControlIn) { "no encuentro bulk control IN 0x81 en alt0" }
        checkNotNull(endpointControlOut) { "no encuentro bulk control OUT 0x01 en alt0" }
        checkNotNull(endpointTsIn) { "no encuentro bulk TS IN 0x82 en alt0" }

        val manager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        val conn = manager.openDevice(device)
            ?: throw IOException("openDevice() = null (falta permiso USB)")
        if (!conn.claimInterface(intf, true)) {
            conn.close()
            throw IOException("claimInterface(0) falló")
        }
        connection = conn
        usbInterface = intf
    }

    /** DVBsky generic bulk control: OUT request followed by exact-size IN reply. */
    fun control(request: ByteArray, responseLength: Int): ByteArray {
        val conn = connection ?: throw IllegalStateException("transporte T230 cerrado")
        val out = endpointControlOut ?: error("sin endpoint control OUT")
        val sent = conn.bulkTransfer(out, request, request.size, CONTROL_TIMEOUT_MS)
        if (sent != request.size) throw IOException("bulk OUT 0x01: $sent/${request.size}")
        if (responseLength == 0) return ByteArray(0)
        val input = endpointControlIn ?: error("sin endpoint control IN")
        val response = ByteArray(responseLength)
        val read = conn.bulkTransfer(input, response, response.size, CONTROL_TIMEOUT_MS)
        if (read != response.size) throw IOException("bulk IN 0x81: $read/${response.size}")
        return response
    }

    /** Reads TS from endpoint 0x82 (bulk, alternate setting 0 per dvbsky.c). */
    fun readTs(buffer: ByteArray, timeoutMs: Int = TS_TIMEOUT_MS): Int {
        val conn = connection ?: throw IllegalStateException("transporte T230 cerrado")
        val ep = endpointTsIn ?: error("sin endpoint TS IN")
        return conn.bulkTransfer(ep, buffer, buffer.size, timeoutMs)
    }

    fun close() {
        try { usbInterface?.let { connection?.releaseInterface(it) } } catch (_: Throwable) { }
        try { connection?.close() } catch (_: Throwable) { }
        connection = null
        usbInterface = null
        endpointControlIn = null
        endpointControlOut = null
        endpointTsIn = null
    }

    companion object {
        const val VID = 0x0572
        const val PID = 0xc688
        private const val EP_CONTROL_OUT = 0x01
        private const val EP_CONTROL_IN = 0x81
        private const val EP_TS_IN = 0x82
        private const val CONTROL_TIMEOUT_MS = 1500
        private const val TS_TIMEOUT_MS = 1000
    }
}
