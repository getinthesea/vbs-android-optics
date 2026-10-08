package nz.vbs.androidoptics

import java.io.DataInputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

/**
 * Sends pose packets to VBSAndroidOptics.dll on the PC (layout: plugin/protocol.h, PosePacket).
 *   host = PC IP address -> UDP over Wi-Fi
 *   host = "usb"         -> TCP to 127.0.0.1, tunnelled to the PC by "adb reverse tcp:47830 tcp:47830"
 * Packets go through a small queue to a background thread, so the ARCore frame loop never waits on the network.
 */
class PoseLink(val host: String) {
    companion object {
        const val PORT = 47830
        const val USB = "usb"
        private const val PACKET_SIZE = 40
        private const val ACK_SIZE = 48
    }

    val sent = AtomicInteger()
    @Volatile var pcReceived = 0; private set
    @Volatile var lastAckMs = 0L; private set
    @Volatile var error: String? = null; private set
    // The rangefinder display the PC sends back in its acks (plugin/lrf.cpp)
    @Volatile var lrfLeft = ""; private set
    @Volatile var lrfRight = ""; private set
    @Volatile var lrfMark = false; private set
    @Volatile var lrfGrid = ""; private set // the lased grid after both buttons (MGRS), "" otherwise

    private val queue = ArrayBlockingQueue<ByteArray>(4)
    @Volatile private var running = true
    private val thread = Thread({ if (host == USB) runTcp() else runUdp() }, "PoseLink").apply { start() }

    /**
     * q is ARCore's rotationQuaternion: x, y, z, w; fovDeg is the wanted horizontal field of view;
     * buttons are held now: bit 0 Bearing, bit 1 Range; calibrating: calibration mode (the IG shows a +)
     */
    fun send(seq: Int, timestampNs: Long, q: FloatArray, tracking: Boolean, calibrate: Int, fovDeg: Float, buttons: Int,
             calibrating: Boolean) {
        val buf = ByteBuffer.allocate(PACKET_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        buf.put("VAO2".toByteArray(Charsets.US_ASCII))
        buf.putInt(seq)
        buf.putLong(timestampNs)
        buf.putFloat(q[0]).putFloat(q[1]).putFloat(q[2]).putFloat(q[3])
        buf.put((if (tracking) 1 else 0).toByte())
        buf.put(calibrate.toByte())
        buf.putShort((fovDeg * 100).toInt().coerceIn(0, 65535).toShort())
        buf.put(buttons.toByte())
        buf.put((if (calibrating) 1 else 0).toByte()) // flags: bit 0 calibrating
        // Keep only the newest poses if the network falls behind
        while (!queue.offer(buf.array())) queue.poll()
    }

    fun close() {
        running = false
        thread.interrupt()
    }

    private fun onAck(b: ByteArray) {
        if (String(b, 0, 4, Charsets.US_ASCII) != "VAA3") return
        pcReceived = ByteBuffer.wrap(b, 8, 4).order(ByteOrder.LITTLE_ENDIAN).int
        lastAckMs = System.currentTimeMillis()
        lrfLeft = String(b, 12, 8, Charsets.US_ASCII).substringBefore('\u0000')
        lrfRight = String(b, 20, 8, Charsets.US_ASCII).substringBefore('\u0000')
        lrfMark = b[28].toInt() != 0
        lrfGrid = String(b, 32, 16, Charsets.US_ASCII).substringBefore(0.toChar())
    }

    private fun runUdp() {
        try {
            DatagramSocket().use { socket ->
                val address = InetAddress.getByName(host)
                Thread({
                    val b = ByteArray(ACK_SIZE)
                    val p = DatagramPacket(b, b.size)
                    while (running) {
                        try {
                            socket.receive(p)
                            if (p.length == ACK_SIZE) onAck(b)
                        } catch (e: Exception) {
                            if (!running || socket.isClosed) break
                        }
                    }
                }, "PoseLinkAck").apply { isDaemon = true; start() }
                error = null
                while (running) {
                    val data = queue.poll(250, TimeUnit.MILLISECONDS) ?: continue
                    socket.send(DatagramPacket(data, data.size, address, PORT))
                    sent.incrementAndGet()
                }
            }
        } catch (e: InterruptedException) {
        } catch (e: Exception) {
            error = "Wi-Fi: ${e.message}"
        }
    }

    private fun runTcp() {
        while (running) {
            try {
                Socket().use { socket ->
                    socket.tcpNoDelay = true
                    socket.connect(InetSocketAddress("127.0.0.1", PORT), 1000)
                    error = null
                    val input = DataInputStream(socket.getInputStream())
                    Thread({
                        val b = ByteArray(ACK_SIZE)
                        try {
                            while (running) {
                                input.readFully(b)
                                onAck(b)
                            }
                        } catch (e: Exception) {
                        }
                    }, "PoseLinkAck").apply { isDaemon = true; start() }
                    val out = socket.getOutputStream()
                    while (running) {
                        val data = queue.poll(250, TimeUnit.MILLISECONDS) ?: continue
                        out.write(data)
                        sent.incrementAndGet()
                    }
                }
            } catch (e: InterruptedException) {
                return
            } catch (e: Exception) {
                error = "USB: ${e.message} (is VBS running and adb reverse tcp:47830 set?)"
                try { Thread.sleep(1000) } catch (e: InterruptedException) { return }
            }
        }
    }
}
