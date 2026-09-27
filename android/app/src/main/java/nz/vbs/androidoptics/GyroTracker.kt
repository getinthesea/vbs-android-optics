package nz.vbs.androidoptics

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Handler
import android.os.HandlerThread
import kotlin.math.abs
import kotlin.math.acos
import kotlin.math.cos
import kotlin.math.sin

/** 3x3 rotations, row-major FloatArray(9) */
object Rot {
    val IDENTITY = floatArrayOf(1f, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 1f)

    fun mul(a: FloatArray, b: FloatArray): FloatArray {
        val r = FloatArray(9)
        for (i in 0..2) for (j in 0..2) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j]
        return r
    }

    fun transpose(a: FloatArray) = floatArrayOf(a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8])

    /** Quaternion (x, y, z, w) to rotation matrix */
    fun fromQuat(q: FloatArray): FloatArray {
        val (x, y, z, w) = q
        return floatArrayOf(
            1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
            2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
            2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y))
    }

    /** Rotation about Z by k quarter turns */
    fun quarterTurnsZ(k: Int): FloatArray {
        val a = k * Math.PI / 2
        val c = cos(a).toFloat()
        val s = sin(a).toFloat()
        return floatArrayOf(c, -s, 0f, s, c, 0f, 0f, 0f, 1f)
    }

    /** Rotation angle in degrees */
    fun angleDeg(a: FloatArray): Double {
        val c = ((a[0] + a[4] + a[8] - 1) / 2).toDouble().coerceIn(-1.0, 1.0)
        return Math.toDegrees(acos(c))
    }
}

/**
 * The phone's gyro-based orientation (Android's game rotation vector: fast, smooth, no magnetometer),
 * kept as a short history so it can be looked up at camera-frame timestamps.
 */
class GyroTracker(context: Context) : SensorEventListener {
    private val sensors = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
    private val sensor: Sensor? = sensors.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR)
    val available get() = sensor != null

    private class Sample(val t: Long, val r: FloatArray)
    private val history = ArrayDeque<Sample>()
    private val lock = Any()

    // 100 Hz on our own thread. At SENSOR_DELAY_FASTEST on the main thread, ARCore's own IMU feed was starved
    // ("IMU buffer is empty", a few samples a second) and it never started tracking.
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
        synchronized(lock) {
            history.addLast(Sample(e.timestamp, r))
            while (history.size > 500) history.removeFirst()
        }
    }

    override fun onAccuracyChanged(s: Sensor?, accuracy: Int) {}

    fun latest(): FloatArray? = synchronized(lock) { history.lastOrNull()?.r }

    /** Orientation nearest to timestamp t (same clock as camera frames); latest if t is out of range */
    fun at(t: Long): FloatArray? = synchronized(lock) {
        var best: Sample? = null
        for (s in history) if (best == null || abs(s.t - t) < abs(best.t - t)) best = s
        if (best == null || abs(best.t - t) > 100_000_000L) history.lastOrNull()?.r else best.r
    }
}

/**
 * Works out how the gyro's axes (the phone's natural portrait axes) sit relative to ARCore's display axes,
 * by comparing how far each says the phone turned. The answer is one of four quarter turns about the screen normal.
 */
class AxisMatcher {
    private val error = DoubleArray(4)
    private var samples = 0
    private var prevArcore: FloatArray? = null
    private var prevGyro: FloatArray? = null
    var quarterTurns = 1; private set // Landscape guess until the phone has moved
    val settled get() = samples >= 5

    /** arcore: display -> world, gyro: device -> world, at the same moment */
    fun add(arcore: FloatArray, gyro: FloatArray) {
        val pa = prevArcore
        val pg = prevGyro
        if (pa == null || pg == null) { prevArcore = arcore; prevGyro = gyro; return }
        val turnArcore = Rot.mul(Rot.transpose(pa), arcore) // relative turn in display axes
        if (Rot.angleDeg(turnArcore) < 3.0) return // Wait for a clear movement
        val turnGyro = Rot.mul(Rot.transpose(pg), gyro)      // relative turn in device axes
        for (k in 0..3) {
            val a = Rot.quarterTurnsZ(k)
            val predicted = Rot.mul(Rot.mul(a, turnGyro), Rot.transpose(a))
            error[k] += Rot.angleDeg(Rot.mul(Rot.transpose(turnArcore), predicted))
        }
        samples++
        quarterTurns = error.indices.minByOrNull { error[it] }!!
        prevArcore = arcore
        prevGyro = gyro
    }
}
