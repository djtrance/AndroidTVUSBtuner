package com.padtv.siano

data class TvService(
    val programId: Int,
    val pmtPid: Int,
    val serviceType: Int,
    val name: String,
    val provider: String,
    val videoPid: Int,
    val videoStreamType: Int,
    val pcrPid: Int,
    val audioPids: List<Int>,
    val epgTitle: String,
    val epgText: String,
    val nextEpgTitle: String,
    val nextEpgText: String,
    val epgUpdatedAtMs: Long,
) {
    val segmentLabel: String get() = if (serviceType == 0xc0) "1seg" else "13seg"
    val playbackPids: Set<Int> get() = buildSet {
        add(0)
        if (pmtPid >= 0) add(pmtPid)
        if (pcrPid >= 0) add(pcrPid)
        if (videoPid >= 0) add(videoPid)
        addAll(audioPids)
    }
}

/** MPEG-TS PSI/SI reader for PAT, PMT, SDT and EIT present/following. */
class TsMetadataParser {
    private data class Service(
        val programId: Int,
        var pmtPid: Int = -1,
        var serviceType: Int = 0,
        var name: String = "",
        var provider: String = "",
        var videoPid: Int = -1,
        var videoType: Int = 0,
        var pcrPid: Int = -1,
        val audioPids: MutableSet<Int> = linkedSetOf(),
        var epgTitle: String = "",
        var epgText: String = "",
        var nextEpgTitle: String = "",
        var nextEpgText: String = "",
        var epgRunning: Boolean = false,
        var epgUpdatedAtMs: Long = 0,
    )

    private val services = linkedMapOf<Int, Service>()
    private val pmtToProgram = mutableMapOf<Int, Int>()
    private val pendingSections = mutableMapOf<Int, ByteArray>()
    private var carry = ByteArray(0)

    @Synchronized
    fun resetChannel() {
        services.clear()
        pmtToProgram.clear()
        pendingSections.clear()
        carry = ByteArray(0)
    }

    @Synchronized
    fun feed(ts: ByteArray, length: Int = ts.size) {
        if (length <= 0) return
        val bytes = ByteArray(carry.size + length)
        System.arraycopy(carry, 0, bytes, 0, carry.size)
        System.arraycopy(ts, 0, bytes, carry.size, length)
        var off = 0
        while (off + TS_PACKET <= bytes.size) {
            if (bytes[off] != 0x47.toByte()) {
                val next = findSync(bytes, off + 1)
                if (next < 0) break
                off = next
                if (off + TS_PACKET > bytes.size) break
            }
            handlePacket(bytes, off)
            off += TS_PACKET
        }
        carry = bytes.copyOfRange(off, bytes.size)
        if (carry.size > TS_PACKET * 2) carry = ByteArray(0)
    }

    @Synchronized
    fun snapshot(): List<TvService> = services.values
        .filter { it.videoPid >= 0 }
        .map { s ->
            TvService(
                programId = s.programId,
                pmtPid = s.pmtPid,
                serviceType = s.serviceType,
                name = s.name.ifBlank { "Servicio ${s.programId}" },
                provider = s.provider,
                videoPid = s.videoPid,
                videoStreamType = s.videoType,
                pcrPid = s.pcrPid,
                audioPids = s.audioPids.toList(),
                epgTitle = s.epgTitle,
                epgText = s.epgText,
                nextEpgTitle = s.nextEpgTitle,
                nextEpgText = s.nextEpgText,
                epgUpdatedAtMs = s.epgUpdatedAtMs,
            )
        }

    private fun handlePacket(ts: ByteArray, off: Int) {
        val pid = ((ts[off + 1].toInt() and 0x1f) shl 8) or (ts[off + 2].toInt() and 0xff)
        val afc = (ts[off + 3].toInt() ushr 4) and 3
        if (afc and 1 == 0) return
        var payloadOff = off + 4
        if (afc and 2 != 0) payloadOff += 1 + (ts[payloadOff].toInt() and 0xff)
        if (payloadOff >= off + TS_PACKET) return

        val pusi = ts[off + 1].toInt() and 0x40 != 0
        if (pusi) {
            val pointer = ts[payloadOff].toInt() and 0xff
            val sectionOff = payloadOff + 1 + pointer
            if (sectionOff + 3 <= off + TS_PACKET) {
                parseSection(pid, ts, sectionOff, off + TS_PACKET)
            }
            pendingSections.remove(pid)
        } else {
            val pending = pendingSections[pid] ?: return
            val added = ts.copyOfRange(payloadOff, off + TS_PACKET)
            val joined = pending + added
            if (joined.size >= 3) {
                val total = 3 + (((joined[1].toInt() and 0x0f) shl 8) or (joined[2].toInt() and 0xff))
                if (joined.size >= total) {
                    parseSection(pid, joined, 0, total)
                    pendingSections.remove(pid)
                } else {
                    pendingSections[pid] = joined
                }
            } else {
                pendingSections[pid] = joined
            }
        }
    }

    private fun parseSection(pid: Int, bytes: ByteArray, off: Int, end: Int) {
        if (off + 3 > end) return
        val tableId = bytes[off].toInt() and 0xff
        val sectionLength = ((bytes[off + 1].toInt() and 0x0f) shl 8) or
            (bytes[off + 2].toInt() and 0xff)
        val sectionEnd = off + 3 + sectionLength
        if (sectionLength < 4 || sectionEnd > end) {
            if (end > off) pendingSections[pid] = bytes.copyOfRange(off, end)
            return
        }
        when {
            pid == 0 && tableId == 0x00 -> parsePat(bytes, off, sectionEnd)
            pid == 0x11 && (tableId == 0x42 || tableId == 0x46) -> parseSdt(bytes, off, sectionEnd)
            pid == 0x12 && (tableId == 0x4e || tableId == 0x4f) -> parseEit(bytes, off, sectionEnd)
            pmtToProgram.containsKey(pid) && tableId == 0x02 -> parsePmt(pid, bytes, off, sectionEnd)
        }
    }

    private fun parsePat(b: ByteArray, off: Int, end: Int) {
        var q = off + 8
        val entriesEnd = end - 4
        while (q + 4 <= entriesEnd) {
            val program = u16(b, q)
            val pmtPid = ((b[q + 2].toInt() and 0x1f) shl 8) or (b[q + 3].toInt() and 0xff)
            if (program != 0) {
                pmtToProgram[pmtPid] = program
                services.getOrPut(program) { Service(program) }.pmtPid = pmtPid
            }
            q += 4
        }
    }

    private fun parsePmt(pid: Int, b: ByteArray, off: Int, end: Int) {
        val program = pmtToProgram[pid] ?: return
        if (off + 12 > end) return
        val service = services.getOrPut(program) { Service(program) }
        service.pmtPid = pid
        service.pcrPid = ((b[off + 8].toInt() and 0x1f) shl 8) or (b[off + 9].toInt() and 0xff)
        val programInfo = ((b[off + 10].toInt() and 0x0f) shl 8) or (b[off + 11].toInt() and 0xff)
        var q = off + 12 + programInfo
        val streamsEnd = end - 4
        service.videoPid = -1
        service.videoType = 0
        service.audioPids.clear()
        while (q + 5 <= streamsEnd) {
            val type = b[q].toInt() and 0xff
            val elementaryPid = ((b[q + 1].toInt() and 0x1f) shl 8) or (b[q + 2].toInt() and 0xff)
            val esInfo = ((b[q + 3].toInt() and 0x0f) shl 8) or (b[q + 4].toInt() and 0xff)
            if (isVideo(type) && service.videoPid < 0) {
                service.videoPid = elementaryPid
                service.videoType = type
            } else if (isAudio(type)) {
                service.audioPids.add(elementaryPid)
            }
            q += 5 + esInfo
        }
    }

    private fun parseSdt(b: ByteArray, off: Int, end: Int) {
        var q = off + 11
        val loopEnd = end - 4
        while (q + 5 <= loopEnd) {
            val serviceId = u16(b, q)
            val descLength = ((b[q + 3].toInt() and 0x0f) shl 8) or (b[q + 4].toInt() and 0xff)
            var d = q + 5
            val descEnd = minOf(loopEnd, d + descLength)
            val service = services.getOrPut(serviceId) { Service(serviceId) }
            while (d + 2 <= descEnd) {
                val tag = b[d].toInt() and 0xff
                val len = b[d + 1].toInt() and 0xff
                val start = d + 2
                if (start + len > descEnd) break
                if (tag == 0x48 && len >= 3) {
                    service.serviceType = b[start].toInt() and 0xff
                    val providerLength = b[start + 1].toInt() and 0xff
                    val nameLengthPos = start + 2 + providerLength
                    if (nameLengthPos < start + len) {
                        service.provider = decodeText(b, start + 2, providerLength)
                        val nameLength = b[nameLengthPos].toInt() and 0xff
                        if (nameLengthPos + 1 + nameLength <= start + len) {
                            service.name = decodeText(b, nameLengthPos + 1, nameLength)
                        }
                    }
                }
                d = start + len
            }
            q = descEnd
        }
    }

    private fun parseEit(b: ByteArray, off: Int, end: Int) {
        if (off + 18 > end) return
        val serviceId = u16(b, off + 3)
        val service = services.getOrPut(serviceId) { Service(serviceId) }
        var q = off + 14
        val eventsEnd = end - 4
        while (q + 12 <= eventsEnd) {
            val runningStatus = (b[q + 10].toInt() ushr 5) and 7
            val descLength = ((b[q + 10].toInt() and 0x0f) shl 8) or (b[q + 11].toInt() and 0xff)
            var d = q + 12
            val descEnd = minOf(eventsEnd, d + descLength)
            while (d + 2 <= descEnd) {
                val tag = b[d].toInt() and 0xff
                val len = b[d + 1].toInt() and 0xff
                val start = d + 2
                if (start + len > descEnd) break
                if (tag == 0x4d && len >= 5) {
                    val titleLen = b[start + 3].toInt() and 0xff
                    val textLenPos = start + 4 + titleLen
                    if (textLenPos < start + len) {
                        val textLen = b[textLenPos].toInt() and 0xff
                        if (textLenPos + 1 + textLen <= start + len) {
                            val title = decodeText(b, start + 4, titleLen)
                            val text = decodeText(b, textLenPos + 1, textLen)
                            if (runningStatus == 4) {
                                service.epgTitle = title
                                service.epgText = text
                                service.epgRunning = true
                            } else if (title != service.epgTitle) {
                                service.nextEpgTitle = title
                                service.nextEpgText = text
                            }
                            service.epgUpdatedAtMs = System.currentTimeMillis()
                        }
                    }
                }
                d = start + len
            }
            q = descEnd
        }
    }

    private fun decodeText(bytes: ByteArray, offset: Int, length: Int): String {
        val end = minOf(bytes.size, offset + length)
        return bytes.copyOfRange(offset, end).toString(Charsets.ISO_8859_1)
            .replace(Regex("[\\u0000-\\u001f\\u007f]+"), " ").trim()
    }

    private fun isVideo(type: Int): Boolean = type in setOf(0x01, 0x02, 0x10, 0x1b, 0x24, 0x42)
    private fun isAudio(type: Int): Boolean = type in setOf(0x03, 0x04, 0x0f, 0x11, 0x81, 0x87)
    private fun u16(b: ByteArray, off: Int): Int = ((b[off].toInt() and 0xff) shl 8) or (b[off + 1].toInt() and 0xff)

    private fun findSync(bytes: ByteArray, from: Int): Int {
        for (i in from until bytes.size) if (bytes[i] == 0x47.toByte()) return i
        return -1
    }

    companion object {
        private const val TS_PACKET = 188
    }
}
