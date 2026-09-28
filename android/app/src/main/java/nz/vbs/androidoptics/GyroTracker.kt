package nz.vbs.androidoptics

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Handler
import android.os.HandlerThread
import kotlin.math.cos
import kotlin.math.sin

/** 3x3 rotations, row-major FloatArray(9) */
object Rot {
    fun mul(a: FloatArray, b: FloatArray): FloatArray {
        val r = FloatArray(9)
        for (i in 0..2) for (j in 0..2) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j]
        return r
    }

    fun transpose(a: FloatArray) = floatArrayOf(a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8])

    /** Rotation matrix to quaternion (x, y, z, w) */
    fun toQuat(m: FloatArray): FloatArray {
        val trace = m[0] + m[4] + m[8]
        return if (trace > 0) {
            val s = Math.sqrt(trace + 1.0).toFloat() * 2
            floatArrayOf((m[7] - m[5]) / s, (m[2] - m[6]) / s, (m[3] - m[1]) / s, 0.25f * s)
        } else if (m[0] > m[4] && m[0] > m[8]) {
            val s = Math.sqrt(1.0 + m[0] - m[4] - m[8]).toFloat() * 2
            floatArrayOf(0.25f * s, (m[1] + m[3]) / s, (m[2] + m[6]) / s, (m[7] - m[5]) / s)
        } else if (m[4] > m[8]) {
            val s = Math.sqrt(1.0 + m[4] - m[0] - m[8]).toFloat() * 2
            floatArrayOf((m[1] + m[3]) / s, 0.25f * s, (m[5] + m[7]) / s, (m[2] - m[6]) / s)
        } else {
            val s = Math.sqrt(1.0 + m[8] - m[0] - m[4]).toFloat() * 2
            floatArrayOf((m[2] + m[6]) / s, (m[5] + m[7]) / s, 0.25f * s, (m[3] - m[1]) / s)
        }
    }

    /** Android sensor world (X east, Y north, Z up) to the Y-up world ARCore and the plugin use (X east, Y up, Z south) */
    val ANDROID_TO_Y_UP = floatArrayOf(1f, 0f, 0f, 0f, 0f, 1f, 0f, -1f, 0f)

    /** Rotation about Z by k quarter turns */
    fun quarterTurnsZ(k: Int): FloatArray {
        val a = k * Math.PI / 2
        val c = cos(a).toFloat()
        val s = sin(a).toFloat()
        return floatArrayOf(c, -s, 0f, s, c, 0f, 0f, 0f, 1f)
    }
}

/**
 * The phone's gyro-based orientation (Android's game rotation vector: fast, smooth, no magnetometer),
 * for gyro tracking. Reports each new orientation to [onOrientation] on its own thread.
 */
class GyroTracker(context: Context) : SensorEventListener {
    private val sensors = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
    private val sensor: Sensor? = sensors.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR)
    val available get() = sensor != null

    /** Called on the sensor thread with each new orientation (timestamp, device -> world) */
    @Volatile var onOrientation: ((Long, FloatArray) -> Unit)? = null

    // 100 Hz on our own thread, so the main thread and ARCore's own sensor feed are left alone
    private var thread: HandlerThread? = null

    fun start() {
        val s = sensor ?: return
        val t = HandlerThread("GyroTracker").apply { start() }
        thread = t
        sensors.registerListener(this, s, 10_000, Handler(t.looper))
    }

    fun stop() {
        sensors.unregisterListener(this)
        thread?.quitSafely()
        thread = null
    }

    override fun onSensorChanged(e: SensorEvent) {
        val r = FloatArray(9)
        SensorManager.getRotationMatrixFromVector(r, e.values) // device -> world
        onOrientation?.invoke(e.timestamp, r)
    }

    override fun onAccuracyChanged(s: Sensor?, accuracy: Int) {}
}
