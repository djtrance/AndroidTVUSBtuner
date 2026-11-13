package com.padtv.siano

import android.content.Context
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import java.io.IOException

/**
 * Capa USB host: abre el dongle, reclama la interfaz 0 y expone los endpoints
 * bulk. Es el equivalente Android de smsusb_probe().
 *
 * No se hardcodean 0x81/0x02: se descubren del descriptor, igual que el driver
 * de Linux. En el PT230 dan 0x81 (IN) y 0x02 (OUT), pero otros clones varian.
 */
class UsbTransport(private val context: Context, val device: UsbDevice) {

    private var connection: UsbDeviceConnection? = null
    private var usbInterface: UsbInterface? = null

    var endpointIn: UsbEndpoint? = null
        private set
    var endpointOut: UsbEndpoint? = null
        private set

    val isOpen: Boolean get() = connection != null

    fun open() {
        if (isOpen) return
        val manager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        val intf = device.getInterface(0)

        for (i in 0 until intf.endpointCount) {
            val ep = intf.getEndpoint(i)
            if (ep.type != UsbConstants.USB_ENDPOINT_XFER_BULK) continue
            if (ep.direction == UsbConstants.USB_DIR_IN) endpointIn = ep else endpointOut = ep
        }
        checkNotNull(endpointIn) { "el dongle no expone bulk IN en la interfaz 0" }
        checkNotNull(endpointOut) { "el dongle no expone bulk OUT en la interfaz 0" }

        val conn = manager.openDevice(device)
            ?: throw IOException("openDevice() = null (revisa el permiso USB)")
        if (!conn.claimInterface(intf, true)) {
            conn.close()
            throw IOException("claimInterface(0) fallo (alguien mas lo tiene?)")
        }
        connection = conn
        usbInterface = intf
    }

    /** Envia un mensaje Siano completo por bulk OUT. Devuelve bytes escritos o <0. */
    fun bulkOut(data: ByteArray): Int {
        val conn = connection ?: throw IllegalStateException("transporte cerrado")
        val ep = endpointOut ?: throw IllegalStateException("sin endpoint OUT")
        return conn.bulkTransfer(ep, data, data.size, TIMEOUT_MS)
    }

    /** Lee un bloque de TS / mensaje de control. Devuelve bytes leidos o <0. */
    fun bulkIn(buffer: ByteArray): Int {
        val conn = connection ?: throw IllegalStateException("transporte cerrado")
        val ep = endpointIn ?: throw IllegalStateException("sin endpoint IN")
        return conn.bulkTransfer(ep, buffer, buffer.size, TIMEOUT_MS)
    }

    fun close() {
        try {
            usbInterface?.let { connection?.releaseInterface(it) }
        } catch (_: Throwable) {
        }
        try {
            connection?.close()
        } catch (_: Throwable) {
        }
        connection = null
        usbInterface = null
    }

    companion object {
        /** El driver original usa 1 s por transferencia. */
        const val TIMEOUT_MS = 1000

        /** Buffer de recepcion del driver original (16 KiB). */
        const val IN_BUFFER = 0x4000

        fun isPt230(device: UsbDevice): Boolean =
            (device.vendorId == 0x1f4d &&
                (device.productId == 0xa230 || device.productId == 0xa270)) ||
                (device.vendorId == 0x3275 && device.productId == 0x0080)

        fun isT230(device: UsbDevice): Boolean =
            device.vendorId == T230UsbTransport.VID && device.productId == T230UsbTransport.PID
    }
}
