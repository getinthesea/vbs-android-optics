package nz.vbs.androidoptics

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.view.Surface
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit

/**
 * Receives VBSAndroidOptics.dll's video (layout: plugin/protocol.h, VideoPacketHeader) and decodes it onto [surface].
 * Frames are rebuilt from UDP packets; a lost packet skips to the next keyframe and asks the PC for one.
 */
class VideoStream(private val surface: Surface) {
    companion object {
        const val PORT = 47831
        private const val HEADER = 16
    }

    /** When each frame finished arriving, for the receive-to-screen figure */
    private val receivedNs = ConcurrentHashMap<Long, Long>()
    @Volatile var videoWidth = 0; private set
    @Volatile var videoHeight = 0; private set
    @Volatile var framesDecoded = 0; private set
    @Volatile var framesDropped = 0; private set
    @Volatile var keyRequests = 0; private set
    @Volatile var decodeMsAvg = 0.0; private set
    @Volatile var lastFrameNs = 0L; private set
    @Volatile var error: String? = null; private set

    private class AccessUnit(val frame: Long, val data: ByteArray, val keyframe: Boolean)
    private class Assembly(val count: Int, val keyframe: Boolean) {
        val parts = arrayOfNulls<ByteArray>(count)
        var have = 0
    }

    @Volatile private var running = true
    private val socket = DatagramSocket(PORT).apply { receiveBufferSize = 4 shl 20 }
    private val queue = ArrayBlockingQueue<AccessUnit>(4)
    private val receiver = Thread(::receive, "VideoReceive").apply { start() }
    private val decoder = Thread(::decode, "VideoDecode").apply { start() }

    fun close() {
        running = false
        socket.close()
        receiver.interrupt()
        decoder.interrupt()
    }

    // ---- Network: rebuild frames ----

    private var pc: InetAddress? = null
    private var lastKeyRequestNs = 0L
    private var waitingForKey = true

    private fun requestKeyframe(lastFrame: Long) {
        val to = pc ?: return
        val now = System.nanoTime()
        if (now - lastKeyRequestNs < 200_000_000) return
        lastKeyRequestNs = now
        val b = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
        b.put("VAK1".toByteArray(Charsets.US_ASCII)).putInt(lastFrame.toInt())
        try { socket.send(DatagramPacket(b.array(), 8, to, PORT)) } catch (e: Exception) { }
        keyRequests++
    }

    private fun receive() {
        val buf = ByteArray(2048)
        val packet = DatagramPacket(buf, buf.size)
        val assemblies = HashMap<Long, Assembly>()
        var lastDelivered = -1L
        while (running) {
            try {
                socket.receive(packet)
            } catch (e: Exception) {
                if (running) error = "Video: ${e.message}"
                break
            }
            if (packet.length < HEADER) continue
            val h = ByteBuffer.wrap(buf, 0, packet.length).order(ByteOrder.LITTLE_ENDIAN)
            if (buf[0] != 'V'.code.toByte() || buf[1] != 'A'.code.toByte() || buf[2] != 'V'.code.toByte() || buf[3] != '1'.code.toByte()) continue
            pc = packet.address
            h.position(4)
            val frame = h.int.toLong() and 0xFFFFFFFFL
            val index = h.short.toInt() and 0xFFFF
            val count = h.short.toInt() and 0xFFFF
            val keyframe = (h.get().toInt() and 1) != 0
            h.get()
            val len = h.short.toInt() and 0xFFFF
            if (frame <= lastDelivered || index >= count || HEADER + len > packet.length) continue

            val a = assemblies.getOrPut(frame) { Assembly(count, keyframe) }
            if (a.parts[index] == null) {
                a.parts[index] = buf.copyOfRange(HEADER, HEADER + len)
                a.have++
            }
            if (a.have < a.count) {
                // Anything two or more frames older than this one is not going to complete
                assemblies.keys.filter { it < frame - 1 }.forEach {
                    assemblies.remove(it)
                    framesDropped++
                    waitingForKey = true
                    requestKeyframe(lastDelivered)
                }
                continue
            }
            assemblies.remove(frame)
            assemblies.keys.filter { it < frame }.forEach { assemblies.remove(it); framesDropped++ }

            if (lastDelivered >= 0 && frame != lastDelivered + 1 && !keyframe) waitingForKey = true
            lastDelivered = frame
            if (waitingForKey && !keyframe) {
                requestKeyframe(frame)
                continue // A P-frame after a gap would decode as garbage
            }
            waitingForKey = false
            val data = ByteArray(a.parts.sumOf { it!!.size })
            var o = 0
            for (p in a.parts) { System.arraycopy(p!!, 0, data, o, p.size); o += p.size }
            receivedNs[frame] = System.nanoTime()
            if (receivedNs.size > 120) receivedNs.keys.filter { it < frame - 60 }.forEach { receivedNs.remove(it) }
            if (!queue.offer(AccessUnit(frame, data, keyframe))) {
                queue.clear() // Decoder fell behind: start again from a keyframe
                waitingForKey = true
                requestKeyframe(frame)
            }
        }
    }

    // ---- Decoder ----

    private fun decode() {
        val codec: MediaCodec
        try {
            codec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, 1280, 720)
            format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 1 shl 20)
            format.setInteger(MediaFormat.KEY_PRIORITY, 0) // Real time
            if (Build.VERSION.SDK_INT >= 30) format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            format.setInteger("vendor.rtc-ext-dec-low-latency.enable", 1) // Samsung Exynos, as Moonlight does
            codec.configure(format, surface, null, 0)
            codec.start()
        } catch (e: Exception) {
            error = "Decoder: ${e.message}"
            return
        }
        val info = MediaCodec.BufferInfo()
        try {
            while (running) {
                val au = queue.poll(5, TimeUnit.MILLISECONDS)
                if (au != null) {
                    val index = codec.dequeueInputBuffer(20_000)
                    if (index >= 0) {
                        codec.getInputBuffer(index)!!.apply { clear(); put(au.data) }
                        codec.queueInputBuffer(index, 0, au.data.size, au.frame,
                            if (au.keyframe) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0)
                    }
                }
                while (true) {
                    val out = codec.dequeueOutputBuffer(info, 0)
                    if (out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                        val f = codec.outputFormat
                        videoWidth = f.getInteger(MediaFormat.KEY_WIDTH)
                        videoHeight = f.getInteger(MediaFormat.KEY_HEIGHT)
                        continue
                    }
                    if (out < 0) break
                    receivedNs[info.presentationTimeUs]?.let {
                        val ms = (System.nanoTime() - it) / 1e6
                        decodeMsAvg = decodeMsAvg * 0.9 + ms * 0.1
                    }
                    codec.releaseOutputBuffer(out, true)
                    framesDecoded++
                    lastFrameNs = System.nanoTime()
                }
            }
        } catch (e: InterruptedException) {
        } catch (e: Exception) {
            error = "Decoder: ${e.message}"
        } finally {
            try { codec.stop() } catch (e: Exception) { }
            codec.release()
        }
    }
}
