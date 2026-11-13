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
import android.os.Handler
import android.os.HandlerThread
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.media3.common.MediaItem
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.source.ProgressiveMediaSource
import androidx.media3.extractor.DefaultExtractorsFactory
import androidx.media3.extractor.ts.TsExtractor
import androidx.media3.ui.PlayerView
import java.io.File
import java.io.IOException
import java.util.concurrent.atomic.AtomicLong

/** Android TV shell: USB tuner, scan list, EPG and a live MPEG-TS player. */
@UnstableApi
class TvActivity : Activity() {

    private data class SignalStats(
        val rfLock: Int,
        val demodLock: Int,
        val snr: Int,
        val rssi: Int,
        val inBand: Int,
    )

    private data class ProgramEntry(
        val channel: Int,
        val frequency: Int,
        val bandwidthMHz: Int,
        val service: TvService,
        val stats: SignalStats?,
        val isIsdbt: Boolean,
    ) {
        val segmentLabel: String get() = if (isIsdbt) service.segmentLabel else "DVB-T2"
    }

    private lateinit var player: ExoPlayer
    private lateinit var playerView: PlayerView
    private lateinit var hub: LiveTsHub
    private lateinit var statusText: TextView
    private lateinit var signalText: TextView
    private lateinit var epgText: TextView
    private lateinit var scanProgressText: TextView
    private lateinit var programList: LinearLayout
    private lateinit var logText: TextView
    private lateinit var dvbBandControls: LinearLayout

    @Volatile private var metadataParser: TsMetadataParser? = null
    private val byteCount = AtomicLong(0)
    private val programs = mutableListOf<ProgramEntry>()
    private var transport: UsbTransport? = null
    private var siano: SianoDevice? = null
    private var t230: T230Device? = null
    @Volatile private var currentUsbDevice: UsbDevice? = null
    @Volatile private var lastStats: SignalStats? = null
    @Volatile private var scanning = false
    @Volatile private var playerActive = false
    private var scanThread: Thread? = null
    private var selectedProgram: ProgramEntry? = null
    private var dvbBandwidthMHz = 8
    private var lastRateBytes = 0L
    private var lastRateTime = 0L

    private val mainHandler by lazy { Handler(mainLooper) }
    private val usbPermissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val device = intent.usbDevice()
            val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            if (granted && device != null) startSupportedDevice(device)
            else appendLog("Permiso USB denegado")
        }
    }

    private val usbDetachReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val detached = intent.usbDevice() ?: return
            val current = currentUsbDevice ?: return
            if (detached.deviceName == current.deviceName) {
                stopReceiver("USB desconectado")
            }
        }
    }

    private val statsThread = HandlerThread("tvusb-control").apply { start() }
    private val statsHandler by lazy { Handler(statsThread.looper) }
    private val statsPoll = object : Runnable {
        override fun run() {
            if (!scanning) {
                try {
                    siano?.requestStatistics()
                    t230?.requestStatistics()
                } catch (e: Throwable) {
                    mainHandler.post { reportUsbFailure("stats USB: ${e.message}") }
                }
            }
            statsHandler.postDelayed(this, 2500)
        }
    }
    private val uiTick = object : Runnable {
        override fun run() {
            refreshStatus()
            refreshSelectedEpg()
            mainHandler.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        buildPlayer()
        buildTvUi()
        registerUsbPermissionReceiver()
        handleUsbIntent(intent)
        mainHandler.post(uiTick)
        statsHandler.postDelayed(statsPoll, 2500)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleUsbIntent(intent)
    }

    override fun onDestroy() {
        mainHandler.removeCallbacksAndMessages(null)
        statsHandler.removeCallbacksAndMessages(null)
        statsThread.quitSafely()
        scanThread?.interrupt()
        hub.finish()
        player.release()
        siano?.stop()
        t230?.stop()
        try {
            unregisterReceiver(usbPermissionReceiver)
            unregisterReceiver(usbDetachReceiver)
        } catch (_: Throwable) {
        }
        super.onDestroy()
    }

    private fun buildPlayer() {
        hub = LiveTsHub()
        player = ExoPlayer.Builder(this).build()
        playerView = PlayerView(this).apply {
            this.player = this@TvActivity.player
            useController = true
            controllerAutoShow = true
            setShowBuffering(PlayerView.SHOW_BUFFERING_ALWAYS)
            setKeepContentOnPlayerReset(true)
            isFocusable = false
        }
    }

    private fun buildTvUi() {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 24, 32, 24)
            setBackgroundColor(0xff101820.toInt())
        }
        val header = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        header.addView(text("TV USB", 26f, 0xffffffff.toInt()),
            LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        statusText = text("Conecta el receptor USB", 18f, 0xffb7d7e5.toInt())
        header.addView(statusText)
        root.addView(header)

        val controls = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        val openButton = button("Abrir receptor") { openFirstSupportedDevice() }
        val scanButton = button("Buscar canales") { scanChannels() }
        val stopButton = button("Detener") { stopPlayback() }
        addControl(controls, openButton)
        addControl(controls, scanButton)
        addControl(controls, stopButton)
        dvbBandControls = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            addControl(this, button("DVB 6 MHz") { setDvbBandwidth(6) })
            addControl(this, button("DVB 8 MHz") { setDvbBandwidth(8) })
            visibility = View.GONE
        }
        controls.addView(dvbBandControls)
        root.addView(controls)

        val content = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        content.addView(playerView, LinearLayout.LayoutParams(
            0, LinearLayout.LayoutParams.MATCH_PARENT, 0.66f))

        val side = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(20, 0, 0, 0)
        }
        signalText = text("Señal: —", 16f, 0xffb7d7e5.toInt())
        side.addView(signalText)
        scanProgressText = text("Canales con video aparecerán aquí", 14f, 0xffb7d7e5.toInt())
        side.addView(scanProgressText)
        val listScroll = ScrollView(this)
        programList = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            isFocusable = false
        }
        listScroll.addView(programList)
        side.addView(listScroll, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))

        epgText = text("EPG: —", 15f, 0xffffffff.toInt()).apply {
            setPadding(10, 10, 10, 10)
            setBackgroundColor(0xff203340.toInt())
            minHeight = 88
        }
        side.addView(epgText)
        content.addView(side, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 0.34f))
        root.addView(content, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))

        logText = text("Listo para iniciar", 12f, 0xff9bb4c0.toInt()).apply {
            maxLines = 4
            setPadding(4, 8, 4, 0)
        }
        root.addView(logText)
        setContentView(root)
        openButton.requestFocus()
    }

    private fun text(value: String, size: Float, color: Int) = TextView(this).apply {
        text = value
        textSize = size
        setTextColor(color)
        gravity = Gravity.CENTER_VERTICAL
    }

    private fun button(label: String, action: () -> Unit) = Button(this).apply {
        text = label
        isFocusable = true
        isFocusableInTouchMode = true
        setPadding(24, 14, 24, 14)
        minHeight = 68
        setBackgroundResource(R.drawable.tv_focus_button)
        setTextColor(resources.getColorStateList(R.color.tv_button_text, theme))
        setOnClickListener { action() }
    }

    private fun addControl(parent: LinearLayout, view: View) {
        parent.addView(view, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT,
        ).apply { marginEnd = 14 })
    }

    private fun handleUsbIntent(intent: Intent?) {
        val device = intent?.usbDevice() ?: return
        if (UsbTransport.isPt230(device) || UsbTransport.isT230(device)) requestPermissionAndStart(device)
    }

    private fun openFirstSupportedDevice() {
        val manager = getSystemService(Context.USB_SERVICE) as UsbManager
        val device = manager.deviceList.values.firstOrNull {
            UsbTransport.isPt230(it) || UsbTransport.isT230(it)
        }
        if (device == null) {
            appendLog("No se detecta PT230/Siano ni T230 DVB-T2")
            return
        }
        requestPermissionAndStart(device)
    }

    private fun requestPermissionAndStart(device: UsbDevice) {
        val manager = getSystemService(Context.USB_SERVICE) as UsbManager
        if (manager.hasPermission(device)) {
            startSupportedDevice(device)
            return
        }
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0
        val pending = PendingIntent.getBroadcast(
            this, 0, Intent(ACTION_USB_PERMISSION).setPackage(packageName), flags)
        manager.requestPermission(device, pending)
        statusText.text = "Esperando permiso USB…"
    }

    private fun startSupportedDevice(device: UsbDevice) {
        when {
            UsbTransport.isPt230(device) -> startWith(device)
            UsbTransport.isT230(device) -> startT230(device)
            else -> appendLog("USB no soportado: %04x:%04x".format(device.vendorId, device.productId))
        }
    }

    private fun startWith(device: UsbDevice) {
        if (siano != null || t230 != null) return
        dvbBandControls.visibility = View.GONE
        programs.clear()
        selectedProgram = null
        val firmware = loadFirmware() ?: run {
            appendLog("No se encontró assets/isdbt_rio.inp")
            return
        }
        val usb = UsbTransport(this, device)
        val parser = TsMetadataParser()
        metadataParser = parser
        val tuner = SianoDevice(usb).apply {
            onLog = { appendLogFromReceiver(it) }
            onTransportError = { error -> mainHandler.post { reportUsbFailure(error) } }
            onTs = { bytes, length ->
                parser.feed(bytes, length)
                byteCount.addAndGet(length.toLong())
                if (playerActive) hub.offer(bytes, length)
            }
            onControl = { type, payload, length ->
                if (type == SianoDevice.MSG_GET_STATISTICS_EX_RES && length >= 0x24) {
                    lastStats = SignalStats(
                        rfLock = le32(payload, 0x0c),
                        demodLock = le32(payload, 0x10),
                        snr = le32(payload, 0x18),
                        rssi = le32(payload, 0x1c),
                        inBand = le32(payload, 0x20),
                    )
                }
                if (type == SianoDevice.MSG_GET_VERSION_EX_RES && length >= 8) {
                    appendLog("Siano chip=0x%04x fw=0x%04x".format(le32(payload, 0), le32(payload, 4)))
                }
            }
        }
        transport = usb
        currentUsbDevice = device
        siano = tuner
        playerActive = true
        hub.begin()
        preparePlayer()
        Thread({
            try {
                tuner.start(firmware)
                val initialChannel = 33
                tuner.tune(freqForChannel(initialChannel), SianoDevice.BW_ISDBT_13SEG)
                runOnUiThread { statusText.text = "PT230 conectado · canal $initialChannel" }
            } catch (e: Throwable) {
                runOnUiThread { statusText.text = "Error de recepción" }
                appendLog("Error iniciando PT230: ${e.message}")
                tuner.stop()
                siano = null
                currentUsbDevice = null
                playerActive = false
            }
        }, "tvusb-start").start()
    }

    private fun startT230(device: UsbDevice) {
        if (t230 != null || siano != null) return
        dvbBandControls.visibility = View.VISIBLE
        programs.clear()
        selectedProgram = null
        val demodFirmware = try {
            assets.open("si2168_b40.fw").use { it.readBytes() }
        } catch (_: IOException) {
            appendLog("Falta assets/si2168_b40.fw")
            return
        }
        val tunerFirmware = try {
            assets.open("si2158_a20.fw").use { it.readBytes() }
        } catch (_: IOException) {
            appendLog("Falta assets/si2158_a20.fw")
            return
        }

        val parser = TsMetadataParser()
        metadataParser = parser
        val frontend = T230Device(T230UsbTransport(this, device)).apply {
            onLog = { appendLog(it) }
            onTs = { bytes, length ->
                parser.feed(bytes, length)
                byteCount.addAndGet(length.toLong())
                if (playerActive) hub.offer(bytes, length)
            }
            onStats = { stats ->
                lastStats = SignalStats(
                    rfLock = if (stats.rfLock) 1 else 0,
                    demodLock = if (stats.demodLock) 1 else 0,
                    snr = stats.snrMilliDb / 1000,
                    rssi = 0,
                    inBand = 0,
                )
            }
        }
        t230 = frontend
        currentUsbDevice = device
        siano = null
        transport = null
        statusText.text = "Iniciando Si2168-B40 / Si2158-A20…"
        Thread({
            try {
                frontend.start(demodFirmware, tunerFirmware)
                runOnUiThread {
                    statusText.text = "T230 DVB-T2 listo · busca canales"
                    scanProgressText.text = "DVB-T2 · banda UHF 474–858 MHz · 8 MHz"
                }
                appendLog("C688 detectado: frontend DVB-T2 inicializado sin sintonía")
            } catch (e: Throwable) {
                runOnUiThread { statusText.text = "Error iniciando T230" }
                appendLog("Error inicializando DVB-T2: ${e.message}")
                frontend.stop()
                t230 = null
                currentUsbDevice = null
            }
        }, "t230-start").start()
    }

    private fun scanChannels() {
        val tuner = siano ?: run {
            if (t230 == null) {
                appendLog("Primero pulsa Abrir receptor")
                return
            }
            null
        }
        val dvbT2 = tuner == null
        if (scanning) return
        scanning = true
        playerActive = false
        player.pause()
        player.stop()
        player.clearMediaItems()
        hub.finish()
        programs.clear()
        runOnUiThread {
            programList.removeAllViews()
            scanProgressText.text = if (dvbT2) "Escaneando DVB-T2 ${dvbBandwidthMHz} MHz… (aprox. 2 min)"
            else "Escaneando ISDB-T canales 14–51… (aprox. 1–2 min)"
        }
        scanThread = Thread({
            val channelRange = if (!dvbT2 || dvbBandwidthMHz == 6) 14..51 else 21..69
            for (channel in channelRange) {
                if (Thread.currentThread().isInterrupted) break
                val parser = metadataParser ?: break
                parser.resetChannel()
                lastStats = null
                val frequency = when {
                    !dvbT2 -> freqForChannel(channel)
                    dvbBandwidthMHz == 6 -> freqForChannel(channel)
                    else -> freqForDvbT2Channel(channel)
                }
                try {
                    if (dvbT2) t230?.tune(frequency, dvbBandwidthMHz, T230Device.DVB_T2)
                    else tuner?.tune(frequency, SianoDevice.BW_ISDBT_13SEG)
                    // SDT/EIT se repiten con distinta periodicidad; esta ventana
                    // permite descubrir servicios y first/following EPG.
                    Thread.sleep(2300)
                    if (dvbT2) t230?.requestStatistics() else tuner?.requestStatistics()
                    Thread.sleep(250)
                } catch (_: InterruptedException) {
                    break
                } catch (e: Throwable) {
                    appendLog("Canal $channel: ${e.message}")
                }
                val stats = lastStats
                val found = parser.snapshot().filter { it.videoPid >= 0 }
                if ((stats?.demodLock == 1 || found.isNotEmpty()) && found.isNotEmpty()) {
                    synchronized(programs) {
                        found.forEach { service -> programs.add(ProgramEntry(channel, frequency, if (dvbT2) dvbBandwidthMHz else 6, service, stats, !dvbT2)) }
                    }
                    runOnUiThread { renderPrograms() }
                }
                runOnUiThread {
                    scanProgressText.text = "${if (dvbT2) "DVB-T2" else "ISDB-T"} $channel · ${found.size} servicios con video"
                }
            }
            scanning = false
            playerActive = false
            runOnUiThread {
            scanProgressText.text = "Escaneo listo · ${programs.size} servicios de video"
                statusText.text = "Elige un servicio de la lista"
            }
        }, "tvusb-scan").also { it.start() }
    }

    private fun renderPrograms() {
        val selected = selectedProgram
        programList.removeAllViews()
        val snapshot = synchronized(programs) { programs.toList() }
            .distinctBy { "${it.channel}-${it.service.programId}" }
            .sortedWith(compareBy<ProgramEntry> { it.channel }.thenBy { it.service.name })
        snapshot.forEach { entry ->
            val label = listOfNotNull(
                "Ch ${entry.channel} · ${entry.service.name} · ${entry.segmentLabel}",
                entry.service.epgTitle.takeIf { it.isNotBlank() }?.let { "Ahora: $it" },
                entry.stats?.let { "SNR ${it.snr} dB · ${it.rssi} dBm" },
            ).joinToString("\n")
            val row = button(label) { playProgram(entry) }
            row.textSize = 14f
            row.minHeight = 64
            row.isSelected = selected?.let {
                entry.channel == it.channel && entry.service.programId == it.service.programId
            } == true
            programList.addView(row, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                bottomMargin = 6
            })
        }
    }

    private fun playProgram(entry: ProgramEntry) {
        val isdbt = siano
        val dvbT2 = t230
        if (isdbt == null && dvbT2 == null) return
        scanning = false
        scanThread?.interrupt()
        selectedProgram = entry
        metadataParser?.resetChannel()
        val pids = linkedSetOf(0, entry.service.pmtPid, entry.service.pcrPid, entry.service.videoPid)
        pids.addAll(entry.service.audioPids)
        player.pause()
        player.stop()
        player.clearMediaItems()
        hub.finish()
        hub.begin(pids)
        playerActive = true
        preparePlayer()
        runOnUiThread {
            statusText.text = "Canal ${entry.channel} · ${entry.service.name} · ${entry.segmentLabel}"
            epgText.text = epgLabel(entry.service)
            renderPrograms()
        }
        Thread({
            try {
                if (entry.isIsdbt) isdbt?.tune(entry.frequency, SianoDevice.BW_ISDBT_13SEG)
                else dvbT2?.tune(entry.frequency, entry.bandwidthMHz, T230Device.DVB_T2)
            } catch (e: Throwable) {
                appendLog("Error sintonizando ${entry.service.name}: ${e.message}")
            }
        }, "tvusb-tune").start()
    }

    private fun preparePlayer() {
        val extractors = DefaultExtractorsFactory().setTsExtractorMode(TsExtractor.MODE_MULTI_PMT)
        val source = ProgressiveMediaSource.Factory(LiveTsDataSourceFactory(hub), extractors)
            .createMediaSource(MediaItem.fromUri("tvusb://live.ts"))
        player.setMediaSource(source)
        player.prepare()
        player.playWhenReady = true
    }

    private fun stopPlayback() {
        scanning = false
        scanThread?.interrupt()
        playerActive = false
        t230?.stopStreaming()
        player.pause()
        player.stop()
        player.clearMediaItems()
        hub.finish()
        statusText.text = "Detenido"
    }

    private fun refreshStatus() {
        val total = byteCount.get()
        val now = System.currentTimeMillis()
        if (lastRateTime != 0L && now > lastRateTime) {
            val kbps = (total - lastRateBytes) * 8 / (now - lastRateTime)
            val stats = lastStats
            val signal = stats?.let {
                "RF ${if (it.rfLock == 1) "lock" else "—"} · Demod ${if (it.demodLock == 1) "lock" else "—"} · SNR ${it.snr} dB · RSSI ${it.rssi} dBm"
            } ?: "Señal: midiendo…"
            runOnUiThread { signalText.text = "$signal · $kbps kbps · ${total / 1024} KiB" }
        }
        lastRateBytes = total
        lastRateTime = now
    }

    private fun refreshSelectedEpg() {
        val selected = selectedProgram ?: return
        val current = metadataParser?.snapshot()?.firstOrNull { it.programId == selected.service.programId } ?: return
        if (current.epgUpdatedAtMs > 0) {
            selectedProgram = selected.copy(service = current)
            runOnUiThread {
                epgText.text = epgLabel(current)
                renderPrograms()
            }
        }
    }

    private fun epgLabel(service: TvService): String = buildString {
        append("Ahora: ").append(service.epgTitle.ifBlank { "EPG sin información" })
        if (service.epgText.isNotBlank()) append("\n").append(service.epgText)
        if (service.nextEpgTitle.isNotBlank()) append("\nDespués: ").append(service.nextEpgTitle)
    }

    private fun setDvbBandwidth(mhz: Int) {
        dvbBandwidthMHz = mhz
        scanProgressText.text = "DVB-T2: banda ${mhz} MHz"
    }

    private fun registerUsbPermissionReceiver() {
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        val detachedFilter = IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(usbPermissionReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
            registerReceiver(usbDetachReceiver, detachedFilter, Context.RECEIVER_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            registerReceiver(usbPermissionReceiver, filter)
            @Suppress("DEPRECATION")
            registerReceiver(usbDetachReceiver, detachedFilter)
        }
    }

    private fun reportUsbFailure(message: String) {
        appendLog("USB: $message")
        statusText.text = "Receptor USB perdido · revisa cable/alimentación"
        signalText.text = "Sin conexión USB"
        stopReceiver(null)
    }

    private fun stopReceiver(message: String?) {
        scanning = false
        scanThread?.interrupt()
        scanThread = null
        playerActive = false
        hub.finish()
        player.pause()
        player.stop()
        player.clearMediaItems()
        lastStats = null
        val oldSiano = siano
        val oldT230 = t230
        siano = null
        t230 = null
        transport = null
        currentUsbDevice = null
        Thread({
            try { oldSiano?.stop() } catch (_: Throwable) { }
            try { oldT230?.stop() } catch (_: Throwable) { }
        }, "tvusb-stop").start()
        if (message != null) statusText.text = message
    }

    private fun loadFirmware(): ByteArray? = try {
        assets.open("isdbt_rio.inp").use { it.readBytes() }
    } catch (_: IOException) {
        getExternalFilesDir(null)?.let { File(it, "isdbt_rio.inp") }
            ?.takeIf { it.exists() }?.readBytes()
    }

    private fun appendLog(line: String) {
        runOnUiThread {
            if (::logText.isInitialized) {
                logText.append("\n$line")
                val lines = logText.text.lines()
                if (lines.size > 9) logText.text = lines.takeLast(9).joinToString("\n")
            }
        }
    }

    private fun freqForChannel(channel: Int): Int = 473_142_857 + (channel - 14) * 6_000_000
    private fun freqForDvbT2Channel(channel: Int): Int = 474_000_000 + (channel - 21) * 8_000_000

    private fun le32(bytes: ByteArray, offset: Int): Int =
        (bytes[offset].toInt() and 0xff) or
            ((bytes[offset + 1].toInt() and 0xff) shl 8) or
            ((bytes[offset + 2].toInt() and 0xff) shl 16) or
            ((bytes[offset + 3].toInt() and 0xff) shl 24)

    private fun Intent.usbDevice(): UsbDevice? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        } else {
            @Suppress("DEPRECATION")
            getParcelableExtra(UsbManager.EXTRA_DEVICE)
        }

    private fun appendLogFromReceiver(line: String) = appendLog(line)

    companion object {
        private const val ACTION_USB_PERMISSION = "com.padtv.siano.USB_PERMISSION"
        private const val DEFAULT_CHANNEL = 33
    }
}
