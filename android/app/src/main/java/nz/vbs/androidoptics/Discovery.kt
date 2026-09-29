package nz.vbs.androidoptics

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException

/**
 * Finds PCs running VBSAndroidOptics.dll on the Wi-Fi network (layout: plugin/protocol.h, DiscoverRequest and
 * DiscoverReply). Broadcasts a request to the pose port every second; each PC replies straight to us, and the
 * reply's source address is that PC. Runs until closed.
 */
class Discovery {
    data class Pc(val name: String, val address: String)

    companion object {
        private const val REPLY_SIZE = 36
    }

    @Volatile var found: List<Pc> = emptyList(); private set
    /** When the first PC answered (0 = none yet), so callers can wait a moment for any others */
    @Volatile var firstReplyMs = 0L; private set
    @Volatile var error: String? = null; private set

    @Volatile private var running = true
    private val thread = Thread(::run, "Discovery").apply { isDaemon = true; start() }

    fun close() {
        running = false
        thread.interrupt()
    }

    private fun run() {
        val request = "VAD1".toByteArray(Charsets.US_ASCII) + ByteArray(4)
        try {
            DatagramSocket().use { socket ->
                socket.broadcast = true
                socket.soTimeout = 250
                val reply = ByteArray(64)
                val packet = DatagramPacket(reply, reply.size)
                var lastSentMs = 0L
                while (running) {
                    val now = System.currentTimeMillis()
                    if (now - lastSentMs >= 1000) {
                        lastSentMs = now
                        for (target in broadcastAddresses()) {
                            try {
                                socket.send(DatagramPacket(request, request.size, target, PoseLink.PORT))
                            } catch (e: Exception) {
                            }
                        }
                    }
                    try {
                        packet.length = reply.size
                        socket.receive(packet)
                    } catch (e: SocketTimeoutException) {
                        continue
                    }
                    if (packet.length != REPLY_SIZE || String(reply, 0, 4, Charsets.US_ASCII) != "VAR1") continue
                    val name = String(reply, 4, 32, Charsets.US_ASCII).substringBefore('\u0000')
                    val address = packet.address.hostAddress ?: continue
                    if (found.none { it.address == address }) {
                        found = found + Pc(name, address)
                        if (firstReplyMs == 0L) firstReplyMs = System.currentTimeMillis()
                    }
                }
            }
        } catch (e: Exception) {
            if (running) error = "Search: ${e.message}"
        }
    }

    /** The all-ones broadcast plus each network's own broadcast address (some routers only pass the latter) */
    private fun broadcastAddresses(): List<InetAddress> {
        val result = mutableListOf(InetAddress.getByName("255.255.255.255"))
        try {
            for (ni in NetworkInterface.getNetworkInterfaces()) {
                if (!ni.isUp || ni.isLoopback) continue
                for (a in ni.interfaceAddresses) a.broadcast?.let { result += it }
            }
        } catch (e: Exception) {
        }
        return result.distinct()
    }
}
