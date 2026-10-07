package nz.vbs.androidoptics

import android.graphics.SurfaceTexture
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

/**
 * Shows the decoded VBS video (or, in calibration mode, the phone's camera: PassthroughCamera), filling the screen
 * (or each half, for Cardboard) in its own shape. GL thread only, but for setBufferSize.
 */
class VideoView {
    lateinit var surface: Surface; private set
    private lateinit var surfaceTexture: SurfaceTexture
    private var texture = 0
    private var program = 0
    @Volatile private var frameAvailable = false
    private var hasFrame = false
    private val texMatrix = FloatArray(16)
    private val turned = FloatArray(16)
    // (u, v) -> (1 - u, 1 - v): the picture turned half round (column-major)
    private val halfTurn = floatArrayOf(-1f, 0f, 0f, 0f, 0f, -1f, 0f, 0f, 0f, 0f, 1f, 0f, 1f, 1f, 0f, 1f)
    private var upsideDown = false
    private val quad: FloatBuffer = ByteBuffer.allocateDirect(8 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
        .apply { put(floatArrayOf(-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f)); position(0) }

    fun create() {
        val t = IntArray(1)
        GLES20.glGenTextures(1, t, 0)
        texture = t[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texture)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        surfaceTexture = SurfaceTexture(texture).apply { setOnFrameAvailableListener { frameAvailable = true } }
        surface = Surface(surfaceTexture)
        program = link(
            """
            attribute vec2 a_Position;
            varying vec2 v_Uv;
            void main() { gl_Position = vec4(a_Position, 0.0, 1.0); v_Uv = a_Position * 0.5 + 0.5; }
            """,
            """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 v_Uv;
            uniform samplerExternalOES u_Texture;
            uniform mat4 u_TexMatrix;
            void main() { gl_FragColor = texture2D(u_Texture, (u_TexMatrix * vec4(v_Uv, 0.0, 1.0)).xy); }
            """)
    }

    /** The size of the frames a camera writes into [surface] */
    fun setBufferSize(width: Int, height: Int) = surfaceTexture.setDefaultBufferSize(width, height)

    /** Latches the newest decoded frame, if any */
    fun update() {
        if (!frameAvailable) return
        frameAvailable = false
        surfaceTexture.updateTexImage()
        surfaceTexture.getTransformMatrix(texMatrix)
        hasFrame = true
    }

    /**
     * Draws the frame side by side [eyes] times (1, or 2 for Cardboard), each filling its share of the view
     * and keeping its shape (the overflow is cropped, centred), moved sideways by [eyeShift] (see Eyes), and
     * magnified [zoom] times about its middle (the calibration camera)
     */
    fun draw(viewWidth: Int, viewHeight: Int, videoWidth: Int, videoHeight: Int, eyes: Int = 1, eyeShift: Float = 0f,
             upsideDown: Boolean = false, zoom: Float = 1f) {
        if (!hasFrame || videoWidth == 0 || videoHeight == 0) return
        this.upsideDown = upsideDown
        val eyeWidth = viewWidth / eyes
        GLES20.glEnable(GLES20.GL_SCISSOR_TEST) // Keeps each eye's cropped overflow out of the other
        for (eye in 0 until eyes) {
            val left = eye * eyeWidth
            GLES20.glScissor(left, 0, eyeWidth, viewHeight)
            drawIn(left + Eyes.offset(eye, eyes, eyeShift).toInt(), eyeWidth, viewHeight, videoWidth, videoHeight, zoom)
        }
        GLES20.glDisable(GLES20.GL_SCISSOR_TEST)
        GLES20.glViewport(0, 0, viewWidth, viewHeight)
    }

    private fun drawIn(left: Int, regionWidth: Int, regionHeight: Int, videoWidth: Int, videoHeight: Int, zoom: Float) {
        val videoAspect = videoWidth.toFloat() / videoHeight
        val regionAspect = regionWidth.toFloat() / regionHeight
        // Fill the region keeping the frame's shape, then magnify about the middle; the scissor crops the rest
        val fillW = if (regionAspect > videoAspect) regionWidth.toFloat() else regionHeight * videoAspect
        val w = (fillW * zoom).toInt()
        val h = (fillW / videoAspect * zoom).toInt()
        GLES20.glViewport(left + (regionWidth - w) / 2, (regionHeight - h) / 2, w, h)
        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texture)
        GLES20.glUniform1i(GLES20.glGetUniformLocation(program, "u_Texture"), 0)
        val matrix = if (upsideDown) turned.also { android.opengl.Matrix.multiplyMM(it, 0, texMatrix, 0, halfTurn, 0) }
            else texMatrix
        GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(program, "u_TexMatrix"), 1, false, matrix, 0)
        val position = GLES20.glGetAttribLocation(program, "a_Position")
        quad.position(0)
        GLES20.glVertexAttribPointer(position, 2, GLES20.GL_FLOAT, false, 0, quad)
        GLES20.glEnableVertexAttribArray(position)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
        GLES20.glDisableVertexAttribArray(position)
    }

    fun release() {
        surface.release()
        surfaceTexture.release()
    }

    private fun link(vertex: String, fragment: String): Int {
        val p = GLES20.glCreateProgram()
        GLES20.glAttachShader(p, compile(GLES20.GL_VERTEX_SHADER, vertex))
        GLES20.glAttachShader(p, compile(GLES20.GL_FRAGMENT_SHADER, fragment))
        GLES20.glLinkProgram(p)
        return p
    }

    private fun compile(type: Int, source: String): Int {
        val s = GLES20.glCreateShader(type)
        GLES20.glShaderSource(s, source)
        GLES20.glCompileShader(s)
        return s
    }
}
