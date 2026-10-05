package nz.vbs.androidoptics

import kotlin.math.abs
import kotlin.math.acos
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * ARCore tracking filled in with the gyro. ARCore gives a drift-free orientation but only once per camera frame
 * (30 a second on most phones), so turning the view in VBS steps; the gyro gives ~100 a second but its heading
 * drifts. Every pose sent comes from the gyro, through a correction that each ARCore frame nudges towards ARCore:
 * smooth like the gyro, held true like ARCore.
 *
 * Orientations are display -> world quaternions (x, y, z, w), Y up, as ARCore's displayOrientedPose and
 * MainActivity's gyro pose. ARCore's world and the gyro's differ by a heading: when ARCore starts (or resumes)
 * tracking, its world is tied to whatever is being sent at that moment, so the view never jumps. Thread-safe:
 * the gyro calls from its sensor thread, ARCore from the GL thread.
 */
class ArGyroFusion(
    private val pull: Float = 0.1f, // share of the gap to ARCore closed per ARCore frame (~0.3 s at 30 a second)
) {
    private var correction = IDENTITY             // gyro world -> what is sent
    private var arToSent: FloatArray? = null      // ARCore world -> what is sent, while ARCore tracks
    private val times = LongArray(HISTORY)        // recent gyro poses, to match ARCore frames by time
    private val poses = Array(HISTORY) { IDENTITY }
    private var next = 0
    private var count = 0

    /** A gyro orientation: returns the orientation to send */
    @Synchronized
    fun gyro(timestampNs: Long, q: FloatArray): FloatArray {
        times[next] = timestampNs
        poses[next] = q
        next = (next + 1) % HISTORY
        if (count < HISTORY) count++
        return normalize(mul(correction, q))
    }

    /** An ARCore frame: its orientation and whether ARCore is tracking */
    @Synchronized
    fun arcore(timestampNs: Long, q: FloatArray, tracking: Boolean) {
        if (!tracking) {
            arToSent = null // when it tracks again, start from wherever the view is then
            return
        }
        val gyroThen = gyroAt(timestampNs) ?: return
        val sentThen = mul(correction, gyroThen)
        val tie = arToSent
        if (tie == null) {
            arToSent = mul(sentThen, inverse(q))
            return
        }
        // How far the gyro has strayed from ARCore, as a rotation of the world; close part of it
        val error = mul(mul(tie, q), inverse(sentThen))
        correction = normalize(mul(scale(error, pull), correction))
    }

    /** The gyro pose nearest an ARCore frame's time; the latest if the clocks do not line up */
    private fun gyroAt(timestampNs: Long): FloatArray? {
        if (count == 0) return null
        var best = (next - 1 + HISTORY) % HISTORY
        val latest = best
        var bestGap = abs(times[best] - timestampNs)
        for (i in 0 until count) {
            val gap = abs(times[i] - timestampNs)
            if (gap < bestGap) { best = i; bestGap = gap }
        }
        return poses[if (bestGap <= MAX_MATCH_NS) best else latest]
    }

    companion object {
        private const val HISTORY = 64              // ~0.6 s of gyro at 100 a second
        private const val MAX_MATCH_NS = 50_000_000L
        private val IDENTITY = floatArrayOf(0f, 0f, 0f, 1f)

        fun mul(a: FloatArray, b: FloatArray) = floatArrayOf(
            a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2])

        fun inverse(q: FloatArray) = floatArrayOf(-q[0], -q[1], -q[2], q[3])

        fun normalize(q: FloatArray): FloatArray {
            val n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3])
            return if (n > 0f) floatArrayOf(q[0] / n, q[1] / n, q[2] / n, q[3] / n) else IDENTITY
        }

        /** The rotation [q] scaled to [t] of its angle (the shorter way round) */
        fun scale(q: FloatArray, t: Float): FloatArray {
            val s = if (q[3] < 0) -1f else 1f // q and -q are the same rotation: take the short one
            val w = (q[3] * s).coerceIn(-1f, 1f)
            val half = acos(w)
            val sinHalf = sin(half)
            if (sinHalf < 1e-6f) return IDENTITY
            val k = sin(half * t) / sinHalf * s
            return floatArrayOf(q[0] * k, q[1] * k, q[2] * k, kotlin.math.cos(half * t))
        }
    }
}
