package nz.vbs.androidoptics

import android.annotation.SuppressLint
import android.content.Context
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.os.Handler
import android.os.HandlerThread
import kotlin.math.abs

/**
 * The back camera into a VideoView, for calibration mode in gyro tracking (in ARCore tracking ARCore owns the
 * camera, and CameraBackground draws its image instead). Opened only while calibrating.
 */
class PassthroughCamera(private val context: Context) {
    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var thread: HandlerThread? = null

    @Volatile var width = 0; private set
    @Volatile var height = 0; private set
    @Volatile var sensorOrientation = 90; private set

    /** Starts the camera into [view]'s surface (camera permission must be granted) */
    @SuppressLint("MissingPermission")
    fun open(view: VideoView) {
        if (device != null) return
        val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val id = manager.cameraIdList.firstOrNull {
            manager.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK
        } ?: return
        val characteristics = manager.getCameraCharacteristics(id)
        sensorOrientation = characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 90
        // A 16:9 preview size near 1280x720: plenty for lining up a crosshair
        val sizes = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            ?.getOutputSizes(android.graphics.SurfaceTexture::class.java) ?: return
        val size = sizes.minByOrNull { abs(it.width - 1280) + abs(it.height - 720) + if (it.width * 9 == it.height * 16) 0 else 10_000 }
            ?: return
        width = size.width
        height = size.height
        view.setBufferSize(width, height)

        val t = HandlerThread("PassthroughCamera").apply { start() }
        thread = t
        val handler = Handler(t.looper)
        manager.openCamera(id, object : CameraDevice.StateCallback() {
            override fun onOpened(camera: CameraDevice) {
                device = camera
                @Suppress("DEPRECATION")
                camera.createCaptureSession(listOf(view.surface), object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(s: CameraCaptureSession) {
                        session = s
                        val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply { addTarget(view.surface) }
                        try { s.setRepeatingRequest(request.build(), null, handler) } catch (e: Exception) {}
                    }
                    override fun onConfigureFailed(s: CameraCaptureSession) {}
                }, handler)
            }
            override fun onDisconnected(camera: CameraDevice) = camera.close()
            override fun onError(camera: CameraDevice, error: Int) = camera.close()
        }, handler)
    }

    fun close() {
        try { session?.close() } catch (e: Exception) {}
        try { device?.close() } catch (e: Exception) {}
        session = null
        device = null
        thread?.quitSafely()
        thread = null
    }

    /** The image needs turning half round for this screen rotation (quarter turns) */
    fun upsideDown(displayTurns: Int): Boolean = (sensorOrientation - displayTurns * 90 + 360) % 360 == 180
}
