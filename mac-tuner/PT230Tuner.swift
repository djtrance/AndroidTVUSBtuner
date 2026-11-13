import AppKit
import AVKit
import SwiftUI

@main
struct PT230TunerApp: App {
    @StateObject private var model = TunerModel()

    var body: some Scene {
        WindowGroup {
            TunerView(model: model)
                .frame(minWidth: 700, minHeight: 780)
        }
        .windowResizability(.contentSize)
    }
}

@MainActor
final class TunerModel: ObservableObject {
    struct ScanChannel: Identifiable {
        var id: String { "\(channel)-\(program)" }
        let channel: Int
        let frequency: Int
        let snr: Int
        let rssi: Int
        let program: Int
        let videoPID: String
        let name: String
        let provider: String
        let segmentType: String
        let epgTitle: String
        let epgText: String
    }

    @Published var channelText = "33"
    @Published var bandwidth = 8
    @Published var state = "Listo"
    @Published var rfStatus = "Sin medir"
    @Published var bitrate = "—"
    @Published var byteCount = "0 B"
    @Published var videoStatus = "Sin pista de video"
    @Published var outputPath: URL?
    @Published var logText = "Conecta el PT230 directamente al Mac.\n"
    @Published var isRunning = false
    @Published var player = AVPlayer()
    @Published var playbackStatus = "Esperando sintonía"
    @Published var hasVideoItem = false
    @Published var isScanning = false
    @Published var scanProgress = ""
    @Published var foundChannels: [ScanChannel] = []
    @Published var tunedService: ScanChannel?

    private struct TuneRequest {
        let channel: Int
        let bandwidth: Int
        let program: Int?
        let segmentType: String?
    }

    private var process: Process?
    private var outputPipe: Pipe?
    private var ffmpegProcess: Process?
    private var ffmpegLogPipe: Pipe?
    private var hlsServer: LocalHLSHTTPServer?
    private var hlsDirectory: URL?
    private var playlistTimer: Timer?
    private var playerObservation: NSKeyValueObservation?
    private var videoSizeObservation: NSKeyValueObservation?
    private var playbackStarted = false
    private var streamWatchdog: Timer?
    private var activePlaylistURL: URL?
    private var signalLost = false
    private var streamWasStalled = false
    private var playerRecoveryPending = false
    private var lastPlayerRecovery = Date.distantPast
    private var lastLiveDataAt = Date()
    private var lastLiveBytes: UInt64 = 0
    private var pendingTune: TuneRequest?
    private var pendingScan = false
    private var pendingText = ""
    private var scanMetrics: [Int: (snr: Int, rssi: Int)] = [:]

    var channel: Int? { Int(channelText) }
    var frequencyText: String {
        guard let channel, (14...51).contains(channel) else { return "Frecuencia: —" }
        let hz = 473_142_857 + (channel - 14) * 6_000_000
        return String(format: "Canal %d · %.3f MHz", channel, Double(hz) / 1_000_000)
    }

    func chooseOutput() {
        let panel = NSSavePanel()
        panel.title = "Guardar transporte MPEG-TS"
        panel.nameFieldStringValue = "captura-ch\(channelText).ts"
        panel.allowedContentTypes = [.data]
        panel.canCreateDirectories = true
        if panel.runModal() == .OK {
            outputPath = panel.url
        }
    }

    func openRecording() {
        guard !isRunning && !isScanning else {
            state = "Detén la recepción o el escaneo antes de abrir una grabación"
            return
        }
        let panel = NSOpenPanel()
        panel.title = "Abrir grabación MPEG-TS"
        panel.allowedContentTypes = [.data]
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let file = panel.url else { return }
        playRecording(file)
    }

    func clearOutput() {
        outputPath = nil
    }

    func tune(channel selectedChannel: Int? = nil, service: ScanChannel? = nil) {
        if let service { channelText = String(service.channel) }
        if let selectedChannel { channelText = String(selectedChannel) }
        guard let channel, (14...51).contains(channel) else {
            state = "Canal fuera de rango (14–51)"
            return
        }
        pendingScan = false
        isScanning = false
        tunedService = service
        let request = TuneRequest(channel: channel, bandwidth: bandwidth,
                                  program: service?.program, segmentType: service?.segmentType)
        if let process, process.isRunning {
            pendingTune = request
            state = "Cambiando a canal \(channel)…"
            process.interrupt()
        } else {
            launch(request)
        }
    }

    func scanChannels() {
        guard !isScanning else { return }
        pendingTune = nil
        foundChannels = []
        scanMetrics = [:]
        tunedService = nil
        scanProgress = "Preparando escaneo…"
        isScanning = true
        if let process, process.isRunning {
            pendingScan = true
            state = "Deteniendo recepción para escanear…"
            process.interrupt()
        } else {
            launchScanner()
        }
    }

    func stop() {
        pendingTune = nil
        pendingScan = false
        isScanning = false
        let stoppingReceiver = process?.isRunning == true
        state = stoppingReceiver ? "Deteniendo…" : "Detenido"
        playbackStatus = stoppingReceiver ? "Cerrando stream…" : "Reproducción detenida"
        player.pause()
        player.replaceCurrentItem(with: nil)
        hasVideoItem = false
        if let process, process.isRunning { process.interrupt() }
        if let ffmpegProcess, ffmpegProcess.isRunning { ffmpegProcess.terminate() }
    }

    private func launch(_ request: TuneRequest) {
        let host = Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/pt230_host")
        let ffmpeg = Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/ffmpeg")
        guard FileManager.default.isExecutableFile(atPath: host.path),
              FileManager.default.isExecutableFile(atPath: ffmpeg.path),
              let firmware = Bundle.main.url(forResource: "isdbt_rio", withExtension: "inp") else {
            state = "Falta un componente de la app"
            appendLog("Error: revisa host, ffmpeg y firmware dentro de PT230 Tuner.app")
            return
        }

        stopPlaybackPipeline(removeFiles: true)
        isScanning = false
        signalLost = false
        streamWasStalled = false
        playerRecoveryPending = false
        lastLiveBytes = 0
        lastPlayerRecovery = .distantPast
        let hz = 473_142_857 + (request.channel - 14) * 6_000_000
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("PT230-Tuner-\(UUID().uuidString)", isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        } catch {
            state = "No se pudo crear el buffer de video"
            appendLog("Error creando HLS temporal: \(error.localizedDescription)")
            return
        }
        hlsDirectory = directory

        let server = LocalHLSHTTPServer()
        hlsServer = server
        server.start(root: directory) { [weak self] result in
            Task { @MainActor in
                switch result {
                case .success(let playlistURL):
                    self?.startPipelines(
                        hostURL: host,
                        ffmpegURL: ffmpeg,
                        firmwareURL: firmware,
                        directory: directory,
                        playlistURL: playlistURL,
                        channel: request.channel,
                        frequency: hz,
                        bandwidth: request.bandwidth,
                        program: request.program,
                        segmentType: request.segmentType
                    )
                case .failure(let error):
                    self?.state = "No se pudo iniciar el reproductor"
                    self?.appendLog("Error del servidor HLS local: \(error.localizedDescription)")
                }
            }
        }
    }

    private func launchScanner() {
        let host = Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/pt230_host")
        guard FileManager.default.isExecutableFile(atPath: host.path),
              let firmware = Bundle.main.url(forResource: "isdbt_rio", withExtension: "inp") else {
            isScanning = false
            state = "Falta el ejecutable o el firmware"
            return
        }
        stopPlaybackPipeline(removeFiles: true)

        let task = Process()
        let pipe = Pipe()
        task.executableURL = host
        var args = ["--auto-fw", "--fw", firmware.path]
        args += ["--scan", "--ch-start", "14", "--ch-end", "51", "--dwell", "2000"]
        task.arguments = args
        task.standardOutput = pipe
        task.standardError = pipe
        pendingText = ""
        task.terminationHandler = { [weak self] ended in
            Task { @MainActor in self?.processEnded(ended) }
        }
        pipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
            Task { @MainActor in self?.consume(text) }
        }

        do {
            try task.run()
            process = task
            outputPipe = pipe
            isRunning = false
            isScanning = true
            state = "Escaneando canales 14–51…"
            scanProgress = "0 de 38 canales"
            appendLog("\n— Escaneo ISDB-T (14–51): tarda aproximadamente 1–2 minutos —")
        } catch {
            pipe.fileHandleForReading.readabilityHandler = nil
            outputPipe = nil
            isScanning = false
            state = "No se pudo iniciar el escaneo"
            appendLog("Error iniciando escaneo: \(error.localizedDescription)")
        }
    }

    private func startPipelines(
        hostURL: URL,
        ffmpegURL: URL,
        firmwareURL: URL,
        directory: URL,
        playlistURL: URL,
        channel: Int,
        frequency: Int,
        bandwidth: Int,
        program: Int?,
        segmentType: String?
    ) {
        let input = Pipe()
        let ffmpegLog = Pipe()

        let ffmpeg = Process()
        ffmpeg.executableURL = ffmpegURL
        var mapArgs: [String]
        if let program {
            mapArgs = ["-map", "0:p:\(program):v:0", "-map", "0:p:\(program):a:0?"]
        } else {
            mapArgs = ["-map", "0:v:0", "-map", "0:a:0?"]
        }
        let videoBitrate = segmentType == "1seg" ? "700k" : "5M"
        let videoMaxRate = segmentType == "1seg" ? "1M" : "7M"
        ffmpeg.arguments = [
            "-hide_banner", "-loglevel", "warning", "-nostdin", "-fflags", "+genpts+discardcorrupt",
            "-probesize", "5000000", "-analyzeduration", "3000000",
            "-i", "pipe:0",
        ] + mapArgs + [
            "-vf", "yadif=mode=send_frame:parity=auto",
            "-c:v", "h264_videotoolbox", "-allow_sw", "1", "-b:v", videoBitrate,
            "-maxrate", videoMaxRate, "-g", "30",
            "-c:a", "aac", "-b:a", "128k", "-ac", "2", "-ar", "48000",
            "-f", "hls", "-hls_time", "2", "-hls_list_size", "6",
            "-hls_flags", "delete_segments+append_list+omit_endlist+independent_segments",
            "-hls_segment_filename", directory.appendingPathComponent("segment_%05d.ts").path,
            directory.appendingPathComponent("live.m3u8").path
        ]
        ffmpeg.standardInput = input.fileHandleForReading
        ffmpeg.standardOutput = FileHandle.nullDevice
        ffmpeg.standardError = ffmpegLog
        ffmpeg.terminationHandler = { [weak self] ended in
            Task { @MainActor in
                guard let self, self.ffmpegProcess === ended else { return }
                self.ffmpegLogPipe?.fileHandleForReading.readabilityHandler = nil
                self.ffmpegLogPipe = nil
                self.ffmpegProcess = nil
                if ended.terminationStatus != 0 {
                    self.playbackStatus = "El reproductor terminó con error"
                }
            }
        }
        ffmpegLog.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
            Task { @MainActor in self?.appendLog("ffmpeg: \(text.trimmingCharacters(in: .whitespacesAndNewlines))") }
        }

        do {
            try ffmpeg.run()
            ffmpegProcess = ffmpeg
            ffmpegLogPipe = ffmpegLog
            try? input.fileHandleForReading.close()
        } catch {
            state = "No se pudo iniciar FFmpeg"
            playbackStatus = "FFmpeg no inició"
            appendLog("Error iniciando ffmpeg: \(error.localizedDescription)")
            hlsServer?.stop()
            hlsServer = nil
            return
        }

        let host = Process()
        let logPipe = Pipe()
        host.executableURL = hostURL
        var args = ["--auto-fw", "--fw", firmwareURL.path]
        args += ["--live", "--ts-stdout", "--freq", String(frequency), "--bw", String(bandwidth)]
        if let outputPath { args += ["--out", outputPath.path] }
        host.arguments = args
        host.standardOutput = input.fileHandleForWriting
        host.standardError = logPipe

        pendingText = ""
        host.terminationHandler = { [weak self] ended in
            Task { @MainActor in self?.processEnded(ended) }
        }
        logPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
            Task { @MainActor in self?.consume(text) }
        }

        do {
            try host.run()
            process = host
            outputPipe = logPipe
            try? input.fileHandleForWriting.close()
            isRunning = true
            state = "Iniciando canal \(channel)…"
            rfStatus = "Midiendo…"
            bitrate = "0 kbps"
            byteCount = "0 B"
            playbackStatus = "Esperando segmentos de video…"
            appendLog("\n— Sintonizando canal \(channel), \(frequency) Hz —")
            watchPlaylist(playlistURL)
            startStreamWatchdog()
        } catch {
            logPipe.fileHandleForReading.readabilityHandler = nil
            outputPipe = nil
            ffmpeg.terminate()
            state = "No se pudo iniciar el host tool"
            appendLog("Error al iniciar pt230_host: \(error.localizedDescription)")
        }
    }

    private func playRecording(_ file: URL) {
        let ffmpegURL = Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/ffmpeg")
        guard FileManager.default.isExecutableFile(atPath: ffmpegURL.path) else {
            state = "No se encontró FFmpeg"
            return
        }
        stopPlaybackPipeline(removeFiles: true)
        tunedService = nil
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("PT230-Playback-\(UUID().uuidString)", isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        } catch {
            state = "No se pudo preparar el reproductor"
            appendLog("Error creando HLS temporal: \(error.localizedDescription)")
            return
        }
        hlsDirectory = directory
        let server = LocalHLSHTTPServer()
        hlsServer = server
        server.start(root: directory) { [weak self] result in
            Task { @MainActor in
                switch result {
                case .success(let playlistURL):
                    self?.startFileTranscoder(ffmpegURL: ffmpegURL, inputFile: file,
                                              directory: directory, playlistURL: playlistURL)
                case .failure(let error):
                    self?.state = "No se pudo iniciar el reproductor"
                    self?.appendLog("Error del servidor HLS local: \(error.localizedDescription)")
                }
            }
        }
    }

    private func startFileTranscoder(ffmpegURL: URL, inputFile: URL,
                                     directory: URL, playlistURL: URL) {
        let ffmpeg = Process()
        let logs = Pipe()
        ffmpeg.executableURL = ffmpegURL
        ffmpeg.arguments = [
            "-hide_banner", "-loglevel", "warning", "-nostdin", "-fflags", "+genpts+discardcorrupt",
            "-probesize", "5000000", "-analyzeduration", "3000000",
            "-i", inputFile.path, "-map", "0:v:0", "-map", "0:a:0?",
            "-vf", "yadif=mode=send_frame:parity=auto",
            "-c:v", "h264_videotoolbox", "-allow_sw", "1", "-b:v", "5M",
            "-maxrate", "7M", "-g", "30",
            "-c:a", "aac", "-b:a", "128k", "-ac", "2", "-ar", "48000",
            "-f", "hls", "-hls_time", "2", "-hls_list_size", "0",
            "-hls_flags", "independent_segments",
            "-hls_segment_filename", directory.appendingPathComponent("segment_%05d.ts").path,
            directory.appendingPathComponent("live.m3u8").path
        ]
        ffmpeg.standardInput = FileHandle.nullDevice
        ffmpeg.standardOutput = FileHandle.nullDevice
        ffmpeg.standardError = logs
        ffmpeg.terminationHandler = { [weak self] ended in
            Task { @MainActor in
                guard let self, self.ffmpegProcess === ended else { return }
                self.ffmpegLogPipe?.fileHandleForReading.readabilityHandler = nil
                self.ffmpegLogPipe = nil
                self.ffmpegProcess = nil
                if ended.terminationStatus != 0 {
                    self.playbackStatus = "No se pudo reproducir el archivo"
                }
            }
        }
        logs.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
            Task { @MainActor in self?.appendLog("ffmpeg: \(text.trimmingCharacters(in: .whitespacesAndNewlines))") }
        }
        do {
            try ffmpeg.run()
            ffmpegProcess = ffmpeg
            ffmpegLogPipe = logs
            isRunning = false
            state = "Reproduciendo grabación"
            playbackStatus = "Preparando video…"
            appendLog("\n— Reproduciendo \(inputFile.lastPathComponent) —")
            watchPlaylist(playlistURL)
        } catch {
            logs.fileHandleForReading.readabilityHandler = nil
            state = "No se pudo iniciar FFmpeg"
            playbackStatus = "FFmpeg no inició"
            appendLog("Error iniciando ffmpeg: \(error.localizedDescription)")
        }
    }

    private func watchPlaylist(_ url: URL) {
        activePlaylistURL = url
        playbackStarted = false
        playlistTimer?.invalidate()
        playlistTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] timer in
            Task { @MainActor in
                guard let self,
                      let playlist = self.hlsDirectory?.appendingPathComponent("live.m3u8"),
                      let text = try? String(contentsOf: playlist, encoding: .utf8),
                      text.contains("#EXTINF:"), !self.playbackStarted else { return }
            self.playbackStarted = true
            timer.invalidate()
            self.playerRecoveryPending = false
            self.startLivePlayerItem(url, status: "Abriendo canal…")
            }
        }
    }

    private func startLivePlayerItem(_ url: URL, status: String) {
        let item = AVPlayerItem(url: url)
        hasVideoItem = true
        playbackStatus = status
        videoStatus = "Buscando cuadros de video…"
        playerObservation = item.observe(\.status, options: [.initial, .new]) { [weak self] item, _ in
            Task { @MainActor in
                guard let self else { return }
                if item.status == .readyToPlay {
                    self.playbackStatus = self.signalLost ? "Esperando señal…" : "Reproduciendo en vivo"
                    self.player.play()
                } else if item.status == .failed {
                    self.videoStatus = "Fallo de decodificación"
                    self.playbackStatus = "Reintentando video…"
                    self.appendLog("AVPlayer: \(item.error?.localizedDescription ?? "error desconocido")")
                    self.recoverLivePlayerIfStalled(force: true)
                }
            }
        }
        videoSizeObservation = item.observe(\.presentationSize, options: [.initial, .new]) { [weak self] item, _ in
            let size = item.presentationSize
            guard size.width > 0, size.height > 0 else { return }
            Task { @MainActor in
                self?.videoStatus = "Video \(Int(size.width))×\(Int(size.height))"
            }
        }
        player.replaceCurrentItem(with: item)
        player.play()
        Task { @MainActor [weak self, weak item] in
            try? await Task.sleep(for: .seconds(8))
            guard let self, let item, self.player.currentItem === item,
                  self.videoStatus == "Buscando cuadros de video…" else { return }
            self.videoStatus = "TS presente, pero sin cuadros decodificados"
            self.appendLog("Diagnóstico: llegó el stream, pero AVPlayer no detectó cuadros de video")
        }
    }

    private func recoverLivePlayerIfStalled(force: Bool = false) {
        guard let url = activePlaylistURL,
              hasVideoItem,
              ffmpegProcess?.isRunning == true,
              Date().timeIntervalSince(lastPlayerRecovery) > 5 else { return }
        let itemFailed = player.currentItem?.status == .failed
        let playbackStalled = player.timeControlStatus != .playing
        guard force || itemFailed || playbackStalled else { return }
        lastPlayerRecovery = Date()
        appendLog("Señal de vuelta; reiniciando AVPlayer desde el live HLS")
        startLivePlayerItem(url, status: "Recuperando reproducción…")
    }

    private func startStreamWatchdog() {
        streamWatchdog?.invalidate()
        lastLiveDataAt = Date()
        streamWatchdog = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self, self.isRunning, self.hasVideoItem else { return }
                if Date().timeIntervalSince(self.lastLiveDataAt) > 5 && !self.streamWasStalled {
                    self.streamWasStalled = true
                    self.state = "Flujo TS interrumpido"
                    self.playbackStatus = "Sin paquetes TS; esperando señal…"
                    self.appendLog("No llegan paquetes TS; el sintonizador permanece abierto")
                }
            }
        }
    }

    private func stopPlaybackPipeline(removeFiles: Bool) {
        playlistTimer?.invalidate()
        playlistTimer = nil
        streamWatchdog?.invalidate()
        streamWatchdog = nil
        playerObservation = nil
        videoSizeObservation = nil
        playbackStarted = false
        activePlaylistURL = nil
        signalLost = false
        streamWasStalled = false
        playerRecoveryPending = false
        player.pause()
        player.replaceCurrentItem(with: nil)
        hasVideoItem = false
        playbackStatus = "Esperando sintonía"

        if let ffmpegProcess, ffmpegProcess.isRunning { ffmpegProcess.terminate() }
        ffmpegProcess = nil
        ffmpegLogPipe?.fileHandleForReading.readabilityHandler = nil
        ffmpegLogPipe = nil
        hlsServer?.stop()
        hlsServer = nil

        if removeFiles, let hlsDirectory {
            try? FileManager.default.removeItem(at: hlsDirectory)
            self.hlsDirectory = nil
        }
    }

    func close() {
        pendingTune = nil
        if let process, process.isRunning { process.interrupt() }
        stopPlaybackPipeline(removeFiles: true)
    }

    private func processEnded(_ ended: Process) {
        guard process === ended else { return }
        outputPipe?.fileHandleForReading.readabilityHandler = nil
        outputPipe = nil
        process = nil
        isRunning = false
        if let next = pendingTune {
            pendingTune = nil
            launch(next)
        } else if pendingScan {
            pendingScan = false
            launchScanner()
        } else if isScanning {
            isScanning = false
            scanProgress = "Escaneo terminado: \(foundChannels.count) canales con video"
            state = foundChannels.isEmpty ? "No se encontraron canales con video" : "Escaneo terminado"
        } else if ended.terminationStatus == 0 {
            state = "Detenido"
            playbackStatus = "Stream detenido"
        } else if ended.terminationReason == .exit {
            state = "El host terminó (código \(ended.terminationStatus))"
        }
    }

    private func consume(_ text: String) {
        pendingText += text
        let lines = pendingText.components(separatedBy: .newlines)
        pendingText = lines.last ?? ""
        for line in lines.dropLast() where !line.isEmpty {
            appendLog(line)
            if line.hasPrefix("[SCAN]") {
                consumeScanResult(line)
            } else if line.hasPrefix("[SERVICE]") {
                consumeService(line)
            } else if line.hasPrefix("  ch ") {
                scanProgress = line.trimmingCharacters(in: .whitespaces)
            } else if line.hasPrefix("[RF]") {
                let values = keyValues(in: line)
                let demodLocked = values["demod_lock"] == "1"
                rfStatus = "\(demodLocked ? "LOCK" : "sin lock") · SNR \(values["snr"] ?? "?") dB · RSSI \(values["rssi"] ?? "?") dBm · banda \(values["inband"] ?? "?") dBm"
                if demodLocked {
                    if signalLost { playerRecoveryPending = true }
                    signalLost = false
                    state = "Sintonizado"
                    if hasVideoItem { playbackStatus = "Señal recuperada…" }
                } else {
                    signalLost = true
                    state = "Interferencia o señal débil…"
                    if hasVideoItem { playbackStatus = "Señal interrumpida; esperando recuperación…" }
                }
            } else if line.hasPrefix("[LIVE]") {
                let values = keyValues(in: line)
                if let b = UInt64(values["bytes"] ?? "") {
                    byteCount = formatBytes(b)
                    let gotNewTs = b > lastLiveBytes
                    lastLiveBytes = b
                    if b > 0 && rfStatus.contains("sin lock") {
                        rfStatus = "TS activo · hay datos de programa"
                        state = "Sintonizado (TS activo)"
                    }
                    if gotNewTs {
                        lastLiveDataAt = Date()
                        if streamWasStalled {
                            streamWasStalled = false
                            playerRecoveryPending = true
                        }
                    }
                    if gotNewTs && playerRecoveryPending && hasVideoItem {
                        let forceRecovery = playerRecoveryPending
                        playerRecoveryPending = false
                        recoverLivePlayerIfStalled(force: forceRecovery)
                    }
                }
                if let kbps = values["kbps"] { bitrate = "\(kbps) kbps" }
            } else if line.contains("no encontre el dongle") || line.contains("libusb_open:") {
                state = "Dongle no disponible"
            } else if line.contains("bulk OUT:") || line.contains("claim interface") {
                state = "Error de USB"
            }
        }
    }

    private func consumeScanResult(_ line: String) {
        let values = keyValues(in: line)
        guard let channel = Int(values["ch"] ?? "") else { return }
        scanMetrics[channel] = (Int(values["snr"] ?? "") ?? 0,
                                Int(values["rssi"] ?? "") ?? 0)
        let count = Int(values["video_count"] ?? "0") ?? 0
        let done = channel - 13
        scanProgress = "Canal \(channel): \(count) servicios con video · \(done)/38"
    }

    private func consumeService(_ line: String) {
        let values = keyValues(in: line)
        guard let channel = Int(values["ch"] ?? ""),
              let program = Int(values["program"] ?? "") else { return }
        let metrics = scanMetrics[channel] ?? (0, 0)
        func decoded(_ key: String) -> String {
            let value = values[key] ?? ""
            return value.removingPercentEncoding ?? value
        }
        let result = ScanChannel(
            channel: channel,
            frequency: Int(values["freq"] ?? "") ?? 0,
            snr: metrics.snr,
            rssi: metrics.rssi,
            program: program,
            videoPID: values["video_pid"] ?? "—",
            name: decoded("name"),
            provider: decoded("provider"),
            segmentType: values["segment"] ?? "13seg",
            epgTitle: decoded("epg_title"),
            epgText: decoded("epg_text")
        )
        foundChannels.removeAll { $0.id == result.id }
        foundChannels.append(result)
        foundChannels.sort {
            if $0.channel != $1.channel { return $0.channel < $1.channel }
            return $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending
        }
    }

    private func keyValues(in line: String) -> [String: String] {
        var result: [String: String] = [:]
        for token in line.split(whereSeparator: \.isWhitespace) {
            let pair = token.split(separator: "=", maxSplits: 1)
            if pair.count == 2 { result[String(pair[0])] = String(pair[1]) }
        }
        return result
    }

    private func formatBytes(_ value: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(value), countStyle: .file)
    }

    private func appendLog(_ line: String) {
        logText += line + "\n"
        if logText.count > 24_000 {
            logText = String(logText.suffix(16_000))
        }
    }
}

struct TunerView: View {
    @ObservedObject var model: TunerModel
    @State private var epgService: TunerModel.ScanChannel?

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(alignment: .firstTextBaseline) {
                Text("PT230 Tuner").font(.largeTitle.bold())
                Spacer()
                Text(model.state)
                    .foregroundStyle(model.isRunning ? .green : .secondary)
                    .font(.headline)
            }

            HStack(spacing: 12) {
                TextField("Canal", text: $model.channelText)
                    .frame(width: 76)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit { model.tune() }
                Text(model.frequencyText)
                    .font(.system(.body, design: .monospaced))
                    .frame(minWidth: 190, alignment: .leading)
                Picker("Ancho", selection: $model.bandwidth) {
                    Text("1 seg").tag(4)
                    Text("13 seg").tag(8)
                }
                .pickerStyle(.segmented)
                .frame(width: 150)
            }

            HStack(spacing: 12) {
                Button("Sintonizar") { model.tune() }
                    .keyboardShortcut(.return, modifiers: [])
                    .disabled(model.isRunning && model.channelText.isEmpty)
                Button(model.isScanning ? "Cancelar escaneo" : "Detener") { model.stop() }
                    .disabled(!model.isRunning && !model.isScanning && !model.hasVideoItem)
                Button(model.isScanning ? "Escaneando…" : "Escanear canales") {
                    model.scanChannels()
                }
                .disabled(model.isScanning)
                Divider().frame(height: 22)
                Button("Abrir TS…") { model.openRecording() }
                Button(model.outputPath == nil ? "Grabar TS…" : "Cambiar archivo…") {
                    model.chooseOutput()
                }
                if model.outputPath != nil {
                    Button("No grabar") { model.clearOutput() }
                }
            }

            ZStack(alignment: .topLeading) {
                VideoPlayer(player: model.player)
                    .frame(maxWidth: .infinity)
                    .frame(height: 340)
                    .background(.black)
                if !model.hasVideoItem {
                    VStack(spacing: 10) {
                        Image(systemName: "tv")
                            .font(.system(size: 42))
                        Text("El video del canal aparecerá aquí")
                        Text(model.playbackStatus)
                            .font(.caption)
                    }
                    .foregroundStyle(.white.opacity(0.8))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(.black)
                }
                Text(model.playbackStatus)
                    .font(.caption.weight(.medium))
                    .foregroundStyle(.white)
                    .padding(.horizontal, 9)
                    .padding(.vertical, 5)
                    .background(.black.opacity(0.65), in: Capsule())
                    .padding(10)
            }
            .clipShape(RoundedRectangle(cornerRadius: 10))

            if let service = model.tunedService {
                HStack(spacing: 8) {
                    Text(service.name).font(.headline)
                    Text(service.segmentType)
                        .font(.caption.weight(.semibold))
                        .padding(.horizontal, 7)
                        .padding(.vertical, 3)
                        .background(.tint.opacity(0.15), in: Capsule())
                    Text(service.epgTitle.isEmpty ? "EPG sin información" : service.epgTitle)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                    Button("Ver EPG") { epgService = service }
                    Spacer()
                }
            }

            if let outputPath = model.outputPath {
                Label(outputPath.path, systemImage: "record.circle")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .help(outputPath.path)
            } else {
                Text("La recepción se muestra en vivo; elige ‘Grabar TS…’ para guardar un .ts.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            HStack(spacing: 24) {
                metric("Señal", value: model.rfStatus)
                metric("Video", value: model.videoStatus)
                metric("Bitrate TS", value: model.bitrate)
                metric("Recibido", value: model.byteCount)
            }
            .padding(12)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(.quaternary.opacity(0.45), in: RoundedRectangle(cornerRadius: 10))

            if model.isScanning || !model.foundChannels.isEmpty {
                GroupBox {
                    VStack(alignment: .leading, spacing: 6) {
                        if model.isScanning {
                            HStack(spacing: 8) {
                                ProgressView().controlSize(.small)
                                Text(model.scanProgress).font(.caption)
                            }
                        }
                        if model.foundChannels.isEmpty {
                            Text(model.isScanning ? "Buscando pistas de video…" : "No se detectaron canales con video")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        } else {
                            ScrollView {
                                VStack(spacing: 2) {
                                    ForEach(model.foundChannels) { result in
                                        HStack(spacing: 8) {
                                            Button {
                                                model.tune(service: result)
                                            } label: {
                                                VStack(alignment: .leading, spacing: 3) {
                                                HStack(spacing: 10) {
                                                    Text("Ch \(result.channel)").bold()
                                                    Text(result.name).fontWeight(.semibold)
                                                    Text(result.segmentType)
                                                        .padding(.horizontal, 6)
                                                        .padding(.vertical, 2)
                                                        .background(.tint.opacity(0.15), in: Capsule())
                                                    Text(String(format: "%.3f MHz", Double(result.frequency) / 1_000_000))
                                                    Text("SNR \(result.snr) dB")
                                                    Text("VID \(result.videoPID)")
                                                    Spacer()
                                                    Text("Sintonizar →").foregroundStyle(.tint)
                                                }
                                                HStack(spacing: 5) {
                                                    Text("EPG:").foregroundStyle(.secondary)
                                                    Text(result.epgTitle.isEmpty ? "sin información" : result.epgTitle)
                                                        .lineLimit(1)
                                                    if !result.epgText.isEmpty {
                                                        Text("· \(result.epgText)")
                                                            .foregroundStyle(.secondary)
                                                            .lineLimit(1)
                                                    }
                                                }
                                                .font(.caption)
                                                }
                                                .font(.system(size: 12, design: .monospaced))
                                                .padding(.vertical, 6)
                                                .padding(.horizontal, 7)
                                                .contentShape(Rectangle())
                                            }
                                            .buttonStyle(.plain)
                                            Button("EPG") { epgService = result }
                                                .buttonStyle(.borderless)
                                        }
                                        .background(.quaternary.opacity(0.35), in: RoundedRectangle(cornerRadius: 5))
                                    }
                                }
                            }
                            .frame(maxHeight: 110)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                } label: {
                    Text("Servicios de video · 1seg y 13seg · \(model.foundChannels.count)").font(.headline)
                }
            }

            Text("Registro").font(.headline)
            ScrollViewReader { proxy in
                ScrollView {
                    Text(model.logText)
                        .font(.system(size: 11, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .textSelection(.enabled)
                        .id("log-end")
                }
                .onChange(of: model.logText) { _, _ in
                    proxy.scrollTo("log-end", anchor: .bottom)
                }
            }
            .padding(8)
            .background(.black.opacity(0.05), in: RoundedRectangle(cornerRadius: 8))

            Text("ISDB-T · canales 14–51 · México: 473.142857 MHz + (canal − 14) × 6 MHz")
                .font(.caption2)
                .foregroundStyle(.tertiary)
        }
        .padding(20)
        .onDisappear { model.close() }
        .sheet(item: $epgService) { service in
            EPGDetailSheet(service: service)
        }
    }

    private func metric(_ title: String, value: String) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title).font(.caption).foregroundStyle(.secondary)
            Text(value).font(.system(.body, design: .rounded).weight(.medium))
        }
    }
}

private struct EPGDetailSheet: View {
    let service: TunerModel.ScanChannel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(alignment: .top) {
                VStack(alignment: .leading, spacing: 5) {
                    Text(service.name).font(.largeTitle.bold())
                    Text("Canal \(service.channel) · \(String(format: "%.3f MHz", Double(service.frequency) / 1_000_000)) · \(service.segmentType)")
                        .foregroundStyle(.secondary)
                    if !service.provider.isEmpty {
                        Text("Proveedor: \(service.provider)").font(.callout)
                    }
                }
                Spacer()
                Text("PID 0x\(service.videoPID)")
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.secondary)
            }

            Divider()
            Text("Guía de programación").font(.title2.bold())
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    Text(service.epgTitle.isEmpty ? "No hay evento EPG disponible en esta emisión." : service.epgTitle)
                        .font(.title3.weight(.semibold))
                        .frame(maxWidth: .infinity, alignment: .leading)
                    if !service.epgText.isEmpty {
                        Text(service.epgText)
                            .font(.body)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    Text("La información se leyó del EIT durante el escaneo; puede cambiar al actualizar la lista.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .padding(.vertical, 4)
            }
            Spacer(minLength: 0)
            HStack {
                Spacer()
                Button("Cerrar") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(24)
        .frame(minWidth: 620, minHeight: 420)
    }
}
