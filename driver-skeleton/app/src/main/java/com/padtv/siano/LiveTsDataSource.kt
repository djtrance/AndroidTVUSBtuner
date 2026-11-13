package com.padtv.siano

import android.net.Uri
import androidx.media3.common.C
import androidx.media3.datasource.DataSource
import androidx.media3.datasource.DataSpec
import androidx.media3.datasource.TransferListener
import java.io.IOException
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit

/** Bounded queue between the USB RX thread and Media3's MPEG-TS extractor. */
class LiveTsHub(private val capacity: Int = 256) {
    internal sealed interface Item {
        data class Bytes(val data: ByteArray) : Item
        data object End : Item
    }

    private val queue = ArrayBlockingQueue<Item>(capacity)
    @Volatile private var active = false
    @Volatile private var selectedPids: Set<Int>? = null
    @Volatile var droppedChunks: Long = 0
        private set

    fun begin(programPids: Set<Int>? = null) {
        queue.clear()
        droppedChunks = 0
        selectedPids = programPids?.toSet()
        active = true
    }

    fun offer(bytes: ByteArray, length: Int = bytes.size) {
        if (!active || length <= 0) return
        val filter = selectedPids
        val payload = if (filter == null) {
            if (length == bytes.size) bytes.copyOf() else bytes.copyOf(length)
        } else {
            val filtered = ByteArray(length)
            var written = 0
            var offset = 0
            while (offset + 188 <= length) {
                if (bytes[offset] == 0x47.toByte()) {
                    val pid = ((bytes[offset + 1].toInt() and 0x1f) shl 8) or
                        (bytes[offset + 2].toInt() and 0xff)
                    if (pid == 0 || filter.contains(pid)) {
                        System.arraycopy(bytes, offset, filtered, written, 188)
                        written += 188
                    }
                }
                offset += 188
            }
            if (written == 0) return
            filtered.copyOf(written)
        }
        val item = Item.Bytes(payload)
        if (!queue.offer(item)) {
            queue.poll()
            droppedChunks++
            queue.offer(item)
        }
    }

    fun finish() {
        active = false
        queue.clear()
        queue.offer(Item.End)
    }

    internal fun take(): Item = try {
        queue.take()
    } catch (e: InterruptedException) {
        Thread.currentThread().interrupt()
        Item.End
    }

    internal fun isActive(): Boolean = active
}

class LiveTsDataSourceFactory(private val hub: LiveTsHub) : DataSource.Factory {
    override fun createDataSource(): DataSource = LiveTsDataSource(hub)
}

private class LiveTsDataSource(private val hub: LiveTsHub) : DataSource {
    private var uri: Uri? = null
    private var current = ByteArray(0)
    private var currentOffset = 0
    private var opened = false

    override fun addTransferListener(transferListener: TransferListener) = Unit

    @Throws(IOException::class)
    override fun open(dataSpec: DataSpec): Long {
        if (dataSpec.position > 0) {
            throw IOException("El stream TS en vivo no permite seek")
        }
        uri = dataSpec.uri
        current = ByteArray(0)
        currentOffset = 0
        opened = true
        return C.LENGTH_UNSET.toLong()
    }

    @Throws(IOException::class)
    override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
        check(opened) { "DataSource no está abierto" }
        if (length == 0) return 0

        while (currentOffset >= current.size) {
            if (!hub.isActive() && current.isEmpty()) return C.RESULT_END_OF_INPUT
            when (val item = hub.take()) {
                is LiveTsHub.Item.Bytes -> {
                    current = item.data
                    currentOffset = 0
                }
                LiveTsHub.Item.End -> return C.RESULT_END_OF_INPUT
            }
        }

        val count = minOf(length, current.size - currentOffset)
        System.arraycopy(current, currentOffset, buffer, offset, count)
        currentOffset += count
        return count
    }

    override fun getUri(): Uri? = uri

    override fun getResponseHeaders(): Map<String, List<String>> = emptyMap()

    override fun close() {
        opened = false
        uri = null
        current = ByteArray(0)
        currentOffset = 0
    }
}
