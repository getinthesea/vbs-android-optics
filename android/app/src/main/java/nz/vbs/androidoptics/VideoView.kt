package nz.vbs.androidoptics

import android.graphics.SurfaceTexture
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

/** Shows the decoded VBS video, letterboxed to its shape. GL thread only. */
class VideoView {
    lateinit var surface: Surface; private set
    private lateinit var surfaceTexture: SurfaceTexture
    private var texture = 0
    private var program = 0
    @Volatile private var frameAvailable = false
    private var hasFrame = false
    private val texMatrix = FloatArray(16)
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

    /** Latches the newest decoded frame, if any */
    fun update() {
        if (!frameAvailable) return
        frameAvailable = false
        surfaceTexture.updateTexImage()
        surfaceTexture.getTransformMatrix(texMatrix)
        hasFrame = true
    }

    /** Draws the frame letterboxed into the view */
    fun draw(viewWidth: Int, viewHeight: Int, videoWidth: Int, videoHeight: Int) {
        if (!hasFrame || videoWidth == 0 || videoHeight == 0) return
        val videoAspect = videoWidth.toFloat() / videoHeight
        val viewAspect = viewWidth.toFloat() / viewHeight
        if (viewAspect > videoAspect) {
            val w = (viewHeight * videoAspect).toInt()
            GLES20.glViewport((viewWidth - w) / 2, 0, w, viewHeight)
        } else {
            val h = (viewWidth / videoAspect).toInt()
            GLES20.glViewport(0, (viewHeight - h) / 2, viewWidth, h)
        }
        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texture)
        GLES20.glUniform1i(GLES20.glGetUniformLocation(program, "u_Texture"), 0)
        GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(program, "u_TexMatrix"), 1, false, texMatrix, 0)
        val position = GLES20.glGetAttribLocation(program, "a_Position")
        quad.position(0)
        GLES20.glVertexAttribPointer(position, 2, GLES20.GL_FLOAT, false, 0, quad)
        GLES20.glEnableVertexAttribArray(position)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
        GLES20.glDisableVertexAttribArray(position)
        GLES20.glViewport(0, 0, viewWidth, viewHeight)
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
