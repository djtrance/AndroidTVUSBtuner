package com.padtv.siano

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.io.File
import java.io.IOException
import java.util.concurrent.atomic.AtomicLong

/**
 * App de prueba: abre el PT230, descarga el firmware y entrega el TS crudo
 * a logcat. No decodifica video; el objetivo es validar el protocolo y medir
 * el bitrate real (1seg vs 13seg).
 */
class MainActivity : Activity() {

    private lateinit var logView: TextView
    private lateinit var statusView: TextView

    private var transport: UsbTransport? = null
    private var siano: SianoDevice? = null

    private val totalBytes = AtomicLong(0)
    private val tsPackets = AtomicLong(0)
    private var lastBytes = 0L
    private var lastTick = 0L

    private val permissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val device = intent.usbDevice()
            val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            log("permiso USB concedido=$granted")
            if (granted && device != null) startWith(device)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
        registerPermissionReceiver()
        handleIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    override fun onDestroy() {
        try {
            unregisterReceiver(permissionReceiver)
        } catch (_: Throwable) {
        }
        siano?.stop()
        super.onDestroy()
    }

    // ---------------------------------------------------------------- UI

    private fun buildUi() {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 24, 24, 24)
        }

        statusView = TextView(this).apply {
            text = "PT230 / Siano SMS2270"
            textSize = 16f
            gravity = Gravity.START
        }
        root.addView(statusView)

        val buttons = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
        }

        buttons.addView(button("Abrir") { openFirstDevice() })
        buttons.addView(button("1seg") { tune(SianoDevice.BW_ISDBT_1SEG) })
        buttons.addView(button("13seg") { tune(SianoDevice.BW_ISDBT_13SEG) })
        buttons.addView(button("Stats") { siano?.requestStatistics() })

        root.addView(buttons)

        logView = TextView(this).apply {
            textSize = 12f
            setTextIsSelectable(true)
        }
        root.addView(ScrollView(this).apply { addView(logView) },
            LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                0,
                1f,
            ))

        setContentView(root)
    }

    private fun button(label: String, onClick: () -> Unit): Button =
        Button(this).apply {
            text = label
            setOnClickListener { onClick() }
        }

    private fun log(line: String) {
        runOnUiThread {
            logView.append(line + "\n")
        }
    }

    private fun updateStatus() {
        val now = System.currentTimeMillis()
        val bytes = totalBytes.get()
        if (lastTick != 0L && now > lastTick) {
            val kbps = ((bytes - lastBytes) * 8 / (now - lastTick)).toInt()
            statusView.text = "TS ${bytes / 1024} KiB · ${tsPackets.get()} paquetes · $kbps kbps"
        }
        lastBytes = bytes
        lastTick = now
    }

    // ------------------------------------------------------------ USB

    private fun handleIntent(intent: Intent?) {
        val device = intent?.usbDevice() ?: return
        if (!UsbTransport.isPt230(device)) return
        registerPermissionAndStart(device)
    }

    private fun openFirstDevice() {
        val manager = getSystemService(Context.USB_SERVICE) as UsbManager
        val device = manager.deviceList.values.firstOrNull { UsbTransport.isPt230(it) }
        if (device == null) {
            log("no hay dongle PT230 conectado")
            return
        }
        registerPermissionAndStart(device)
    }

    private fun registerPermissionAndStart(device: UsbDevice) {
        val manager = getSystemService(Context.USB_SERVICE) as UsbManager
        if (manager.hasPermission(device)) {
            startWith(device)
            return
        }
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            PendingIntent.FLAG_MUTABLE
        } else {
            0
        }
        val pi = PendingIntent.getBroadcast(
            this,
            0,
            Intent(ACTION_USB_PERMISSION).setPackage(packageName),
            flags,
        )
        manager.requestPermission(device, pi)
        log("pidiendo permiso USB...")
    }

    private fun registerPermissionReceiver() {
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(permissionReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            registerReceiver(permissionReceiver, filter)
        }
    }

    private fun startWith(device: UsbDevice) {
        if (siano != null) return
        val firmware = loadFirmware()
        if (firmware == null) {
            log("falta assets/isdbt_rio.inp (ver README)")
            return
        }

        val tx = UsbTransport(this, device)
        val dev = SianoDevice(tx).apply {
            onLog = { log(it) }
            onTs = { buffer, length ->
                totalBytes.addAndGet(length.toLong())
                tsPackets.addAndGet((length / 188).toLong())
                if (System.currentTimeMillis() - lastTick > 1000) updateStatus()
            }
        }

        transport = tx
        siano = dev
        Thread({
            try {
                dev.start(firmware)
                dev.tune(DEFAULT_FREQ_HZ, SianoDevice.BW_ISDBT_13SEG)
            } catch (t: Throwable) {
                log("error: ${t.message}")
                dev.stop()
                siano = null
            }
        }, "siano-start").start()
    }

    private fun tune(bandwidth: Int) {
        val dev = siano ?: run {
            log("abre el dongle primero")
            return
        }
        Thread({
            try {
                dev.tune(DEFAULT_FREQ_HZ, bandwidth)
            } catch (t: Throwable) {
                log("tune error: ${t.message}")
            }
        }, "siano-tune").start()
    }

    private fun loadFirmware(): ByteArray? {
        return try {
            assets.open("isdbt_rio.inp").use { it.readBytes() }
        } catch (_: IOException) {
            val external = File(getExternalFilesDir(null), "isdbt_rio.inp")
            if (external.exists()) external.readBytes() else null
        }
    }

    private fun Intent.usbDevice(): UsbDevice? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        } else {
            @Suppress("DEPRECATION")
            getParcelableExtra(UsbManager.EXTRA_DEVICE)
        }

    companion object {
        private const val ACTION_USB_PERMISSION = "com.padtv.siano.USB_PERMISSION"

        /** Canal 33 probado en el dongle: 587.142857 MHz. */
        private const val DEFAULT_FREQ_HZ = 587_142_857
    }
}
