package nz.vbs.androidoptics

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.graphics.Color
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import com.google.ar.core.ArCoreApk
import com.google.ar.core.CameraConfig
import com.google.ar.core.CameraConfigFilter
import com.google.ar.core.Config
import com.google.ar.core.Session
import com.google.ar.core.TrackingState
import java.util.EnumSet
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * VBS Android Optics: sends this phone's orientation to VBSAndroidOptics.dll, which aims the VBS view with it
 * and streams the view back. Hold the phone like a camera (landscape, screen towards you).
 * Tracking comes from the gyro (instant, fast, heading drifts slowly: re-zero with Calibrate) or ARCore
 * (camera + motion, no drift once tracking, needs sideways movement and detail to start).
 */
class MainActivity : Activity(), GLSurfaceView.Renderer {

    private lateinit var glView: GLSurfaceView
    private lateinit var statusText: TextView
    private lateinit var hostField: EditText
    private val ui = Handler(Looper.getMainLooper())
    private val prefs by lazy { getSharedPreferences("tracker", MODE_PRIVATE) }

    @Volatile private var session: Session? = null
    private var installRequested = false
    @Volatile private var link: PoseLink? = null

    // GL thread state
    private val background = CameraBackground()
    private var video: VideoView? = null
    @Volatile private var videoStream: VideoStream? = null
    private val gyro by lazy { GyroTracker(this) }
    @Volatile private var gyroMode = true   // Tracking source: gyro, or ARCore when false
    @Volatile private var displayTurns = 1  // Screen rotation in quarter turns (device axes -> display axes)
    @Volatile private var videoOn = true // Off: stop receiving/decoding and show the camera (what ARCore sees)

    // Zoom presets: name and horizontal field of view in degrees. The plugin sets VBS's view to match.
    private val ZOOMS = listOf("1x" to 60f, "NVG" to 40f, "4x" to 15f, "7x binos" to 7.6f, "10x" to 5.5f, "15x" to 3.7f)
    @Volatile private var zoom = 3
    private lateinit var crosshair: CrosshairView
    private lateinit var cornerMask: CornerMaskView
    private var textureSetFor: Session? = null
    private var displayChanged = false
    private var viewWidth = 0
    private var viewHeight = 0
    private var lastTimestamp = 0L
    @Volatile private var seq = 0

    @Volatile private var calibrate = 0
    @Volatile private var trackingText = "starting"
    @Volatile private var cameraFps = 30
    @Volatile private var frames = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        // "adb shell am start -n nz.vbs.androidoptics/.MainActivity --es host <PC IP or usb>" sets the link from a script
        intent.getStringExtra("host")?.let { prefs.edit().putString("host", it).apply() }
        zoom = prefs.getInt("zoom", 3).coerceIn(0, ZOOMS.size - 1)
        if (intent.getBooleanExtra("novideo", false)) videoOn = false // --ez novideo true: diagnostics
        gyroMode = prefs.getBoolean("gyroMode", true)

        glView = GLSurfaceView(this).apply {
            preserveEGLContextOnPause = true
            setEGLContextClientVersion(2)
            setRenderer(this@MainActivity)
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
        }

        statusText = TextView(this).apply {
            setTextColor(Color.WHITE)
            textSize = 16f
            typeface = android.graphics.Typeface.MONOSPACE
            setShadowLayer(6f, 0f, 0f, Color.BLACK) // Readable over the camera image
        }
        hostField = EditText(this).apply {
            setText(prefs.getString("host", PoseLink.USB))
            hint = "PC IP address, or usb"
            setTextColor(Color.BLACK)
            setHintTextColor(Color.DKGRAY)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            imeOptions = EditorInfo.IME_ACTION_DONE
            isSingleLine = true
            minEms = 7
            setOnEditorActionListener { _, _, _ -> applyHost(); false }
        }
        val connectButton = Button(this).apply {
            text = "Go"
            setOnClickListener { applyHost() }
        }
        val calibrateButton = Button(this).apply {
            text = "Calibrate"
            setOnClickListener { calibrate = (calibrate + 1) and 0xFF }
        }
        val zoomButton = Button(this).apply {
            text = "Zoom: ${ZOOMS[zoom].first}"
            setOnClickListener {
                zoom = (zoom + 1) % ZOOMS.size
                prefs.edit().putInt("zoom", zoom).apply()
                text = "Zoom: ${ZOOMS[zoom].first}"
            }
        }
        val videoButton = Button(this).apply {
            text = "Video: on"
            setOnClickListener {
                videoOn = !videoOn
                text = if (videoOn) "Video: on" else "Video: off"
            }
        }
        val trackButton = Button(this).apply {
            text = if (gyroMode) "Gyro" else "ARCore"
            setOnClickListener {
                prefs.edit().putBoolean("gyroMode", !gyroMode).apply()
                recreate() // Starts or stops ARCore and the camera
            }
        }
        val controls = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(hostField)
            addView(connectButton)
            addView(trackButton)
            addView(zoomButton)
            addView(videoButton)
        }
        // Everything but Calibrate lives in a panel behind the Stats button (top left), so the view stays clear
        val panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 16, 24, 16)
            setBackgroundColor(0x99000000.toInt())
            visibility = View.GONE
            addView(statusText)
            addView(controls)
        }
        val statsButton = Button(this).apply {
            text = "Settings"
            setOnClickListener {
                panel.visibility = if (panel.visibility == View.VISIBLE) View.GONE else View.VISIBLE
                hideSystemUi()
            }
        }
        // Settings and Calibrate: the same small size, tucked into the black corners of the mask
        val density = resources.displayMetrics.density
        val cornerW = (48 * density).toInt()
        val cornerH = (26 * density).toInt()
        val cornerGap = (3 * density).toInt()
        val topLeft = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(cornerGap, cornerGap, cornerGap, cornerGap)
            addView(statsButton, LinearLayout.LayoutParams(cornerW, cornerH))
            addView(panel, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        }
        for (b in listOf(statsButton, calibrateButton, connectButton, trackButton, zoomButton, videoButton)) styleButton(b)
        for (b in listOf(statsButton, calibrateButton)) {
            b.textSize = 10f
            b.setPadding(0, 0, 0, 0)
            b.minWidth = 0
            b.minHeight = 0
            b.minimumWidth = 0
            b.minimumHeight = 0
        }
        statsButton.layoutParams = LinearLayout.LayoutParams(cornerW, cornerH) // styleButton reset it
        crosshair = CrosshairView(this).apply { visibility = View.GONE }
        cornerMask = CornerMaskView(this)
        setContentView(FrameLayout(this).apply {
            addView(glView)
            addView(cornerMask)
            addView(crosshair)
            addView(topLeft, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.START))
            addView(calibrateButton, FrameLayout.LayoutParams(cornerW, cornerH, Gravity.TOP or Gravity.END).apply { setMargins(cornerGap, cornerGap, cornerGap, cornerGap) })
        })
        ui.post(object : Runnable {
            override fun run() {
                updateStatus()
                ui.postDelayed(this, 250)
            }
        })
    }

    override fun onResume() {
        super.onResume()
        hideSystemUi()
        @Suppress("DEPRECATION")
        displayTurns = windowManager.defaultDisplay.rotation // Surface.ROTATION_0..270 are 0..3
        if (gyroMode) {
            trackingText = if (gyro.available) "gyro - press Calibrate facing forward" else "no gyro on this phone"
            gyro.onOrientation = { t, r -> sendGyroPose(t, r) }
            glView.onResume()
            gyro.start()
            startLink()
            return
        }
        gyro.onOrientation = null
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.CAMERA), 0)
            return
        }
        if (session == null) {
            try {
                if (ArCoreApk.getInstance().requestInstall(this, !installRequested) == ArCoreApk.InstallStatus.INSTALL_REQUESTED) {
                    installRequested = true
                    return
                }
                session = Session(this).also { configure(it) }
            } catch (e: Exception) {
                trackingText = "ARCore unavailable: ${e.message}"
                return
            }
        }
        try {
            session?.resume()
        } catch (e: Exception) {
            trackingText = "Camera unavailable: ${e.message}"
            session = null
            return
        }
        glView.onResume()
        startLink()
    }

    override fun onPause() {
        super.onPause()
        glView.onPause()
        gyro.stop()
        session?.pause()
        link?.close()
        link = null
        videoStream?.close()
        videoStream = null
    }

    override fun onDestroy() {
        super.onDestroy()
        session?.close()
        session = null
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        if (grantResults.firstOrNull() != PackageManager.PERMISSION_GRANTED) {
            trackingText = "Camera permission is needed for tracking"
        }
    }

    private fun configure(s: Session) {
        // 60 fps camera gives 60 poses a second where the phone supports it
        val fast = s.getSupportedCameraConfigs(
            CameraConfigFilter(s).setTargetFps(EnumSet.of(CameraConfig.TargetFps.TARGET_FPS_60)))
        if (fast.isNotEmpty()) {
            s.cameraConfig = fast[0]
            cameraFps = 60
        }
        s.configure(Config(s).apply {
            planeFindingMode = Config.PlaneFindingMode.DISABLED
            lightEstimationMode = Config.LightEstimationMode.DISABLED
            depthMode = Config.DepthMode.DISABLED
            updateMode = Config.UpdateMode.LATEST_CAMERA_IMAGE
            focusMode = Config.FocusMode.AUTO // Fixed focus left the image soft, giving ARCore too few features to lock on
        })
    }

    /** Gyro tracking: send the phone's orientation as the display-oriented pose ARCore would give */
    private fun sendGyroPose(timestampNs: Long, deviceToWorld: FloatArray) {
        val a = Rot.quarterTurnsZ(displayTurns)                           // device axes -> display axes
        val displayToWorld = Rot.mul(Rot.mul(Rot.ANDROID_TO_Y_UP, deviceToWorld), Rot.transpose(a))
        link?.send(seq++, timestampNs, Rot.toQuat(displayToWorld), true, calibrate, ZOOMS[zoom].second)
        frames++
    }

    private fun applyHost() {
        prefs.edit().putString("host", hostField.text.toString().trim().ifEmpty { PoseLink.USB }).apply()
        startLink()
        hideSystemUi()
    }

    private fun startLink() {
        link?.close()
        link = PoseLink(prefs.getString("host", PoseLink.USB) ?: PoseLink.USB)
    }

    private var lastFrames = 0
    private var lastRateCheck = System.currentTimeMillis()
    private var poseRate = 0

    private fun updateStatus() {
        val now = System.currentTimeMillis()
        if (now - lastRateCheck >= 1000) {
            poseRate = ((frames - lastFrames) * 1000 / (now - lastRateCheck)).toInt()
            lastFrames = frames
            lastRateCheck = now
        }
        val l = link
        val linkText = when {
            l == null -> "not connected"
            l.error != null -> l.error
            l.lastAckMs == 0L -> "${l.host}: sent ${l.sent.get()}, no reply from PC yet"
            now - l.lastAckMs > 1000 -> "${l.host}: PC NOT RECEIVING (last reply ${(now - l.lastAckMs) / 1000} s ago)"
            else -> "${l.host}: PC receiving (${l.pcReceived} poses)"
        }
        val v = videoStream
        val videoText = when {
            v == null -> "Video: off"
            v.error != null -> v.error
            v.framesDecoded == 0 -> "Video: waiting for VBS (video needs Wi-Fi, not usb)"
            else -> "Video: ${v.videoWidth}x${v.videoHeight}, ${v.framesDecoded} frames, ${v.framesDropped} dropped, " +
                "${v.keyRequests} key requests, receive-to-screen %.1f ms".format(v.decodeMsAvg)
        }
        crosshair.visibility = if (v != null && v.framesDecoded > 0 && System.nanoTime() - v.lastFrameNs < 1_000_000_000L) View.VISIBLE else View.GONE
        if (v != null) crosshair.setOptics(ZOOMS[zoom].second, v.videoWidth, v.videoHeight)
        val showingVideo = crosshair.visibility == View.VISIBLE && v != null
        cornerMask.setVideoSize(if (showingVideo) v!!.videoWidth else 0, if (showingVideo) v!!.videoHeight else 0)
        val source = if (gyroMode) "Gyro: $trackingText  ($poseRate poses/s)" else "ARCore: $trackingText  ($poseRate poses/s, camera ${cameraFps} fps)"
        statusText.text = "$source\n$linkText\n$videoText"
    }

    /** #444444 buttons with black text */
    private fun styleButton(b: Button) {
        b.background = android.graphics.drawable.GradientDrawable().apply {
            setColor(0xFF444444.toInt())
            cornerRadius = 12f
        }
        b.setTextColor(Color.BLACK)
        b.isAllCaps = false
        b.setPadding(32, 16, 32, 16)
        b.layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT)
            .apply { setMargins(6, 6, 6, 6) }
    }

    @Suppress("DEPRECATION")
    private fun hideSystemUi() {
        window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            or View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
            or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION)
    }

    // ---- GL thread ----

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        background.create()
        textureSetFor = null
        videoStream?.close()
        videoStream = null
        video = VideoView().apply { create() }
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        GLES20.glViewport(0, 0, width, height)
        viewWidth = width
        viewHeight = height
        displayChanged = true
    }

    @Suppress("DEPRECATION")
    override fun onDrawFrame(gl: GL10?) {
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        val s = session // null in gyro mode
        if (s != null && textureSetFor !== s) {
            s.setCameraTextureName(background.texture)
            textureSetFor = s
            displayChanged = true
        }
        if (s != null && displayChanged) {
            s.setDisplayGeometry(windowManager.defaultDisplay.rotation, viewWidth, viewHeight)
            displayChanged = false
        }
        val v = video
        if (!videoOn && videoStream != null) {
            videoStream?.close()
            videoStream = null
        }
        if (videoOn && videoStream == null && v != null) {
            videoStream = try { VideoStream(v.surface) } catch (e: Exception) { null }
        }
        val frame = if (s == null) null else try { s.update() } catch (e: Exception) { return }

        // Show the VBS video while it is arriving, otherwise the camera
        val stream = videoStream
        v?.update()
        if (v != null && stream != null && System.nanoTime() - stream.lastFrameNs < 1_000_000_000L) {
            v.draw(viewWidth, viewHeight, stream.videoWidth, stream.videoHeight)
        } else if (frame != null) {
            background.draw(frame)
        }
        if (frame == null || frame.timestamp == lastTimestamp) return // Gyro mode, or no new camera frame yet
        lastTimestamp = frame.timestamp

        val camera = frame.camera
        val tracking = camera.trackingState == TrackingState.TRACKING
        trackingText = if (tracking) "tracking"
            else "not tracking (${camera.trackingFailureReason}) - move the phone side to side to start"
        // Display-oriented: X right, Y up, looking along -Z; world Y up with gravity
        link?.send(seq++, frame.timestamp, camera.displayOrientedPose.rotationQuaternion, tracking, calibrate, ZOOMS[zoom].second)
        frames++
    }
}
