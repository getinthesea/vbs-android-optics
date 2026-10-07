package nz.vbs.androidoptics

import android.opengl.GLES11Ext
import android.opengl.GLES20
import com.google.ar.core.Coordinates2d
import com.google.ar.core.Frame
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

/**
 * Draws ARCore's camera image, so you can see what the tracker sees: full screen, or (Cardboard, calibration mode)
 * the middle of it once per eye, moved like the eyes' pictures (see Eyes). GL thread only.
 */
class CameraBackground {
    private val quad = floatArrayOf(-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f)
    private val quadCoords: FloatBuffer = floatBuffer(quad)
    private val cropCoords: FloatBuffer = floatBuffer(FloatArray(8)) // the part of the view each eye shows
    private val texCoords: FloatBuffer = floatBuffer(FloatArray(8))
    private var texCoordsEyes = 0
    private var texCoordsZoom = 1f
    private var program = 0
    var texture = 0; private set

    fun create() {
        val t = IntArray(1)
        GLES20.glGenTextures(1, t, 0)
        texture = t[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texture)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        program = link(
            """
            attribute vec4 a_Position;
            attribute vec2 a_TexCoord;
            varying vec2 v_TexCoord;
            void main() { gl_Position = a_Position; v_TexCoord = a_TexCoord; }
            """,
            """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 v_TexCoord;
            uniform samplerExternalOES u_Texture;
            void main() { gl_FragColor = texture2D(u_Texture, v_TexCoord); }
            """)
    }

    /** [zoom] magnifies the middle of the picture (the calibration camera) */
    fun draw(frame: Frame, viewWidth: Int = 0, viewHeight: Int = 0, eyes: Int = 1, eyeShift: Float = 0f, zoom: Float = 1f) {
        if (frame.hasDisplayGeometryChanged() || eyes != texCoordsEyes || zoom != texCoordsZoom) {
            // Each eye shows the middle 1/eyes of the full-screen image, at the same scale, magnified [zoom] times
            val a = 1f / eyes / zoom
            val b = 1f / zoom
            cropCoords.position(0)
            cropCoords.put(floatArrayOf(-a, -b, a, -b, -a, b, a, b))
            cropCoords.position(0)
            frame.transformCoordinates2d(
                Coordinates2d.OPENGL_NORMALIZED_DEVICE_COORDINATES, cropCoords,
                Coordinates2d.TEXTURE_NORMALIZED, texCoords)
            texCoordsEyes = eyes
            texCoordsZoom = zoom
        }
        if (frame.timestamp == 0L) return
        if (eyes < 2 || viewWidth <= 0) {
            drawQuad()
            return
        }
        val eyeWidth = viewWidth / eyes
        GLES20.glEnable(GLES20.GL_SCISSOR_TEST)
        for (eye in 0 until eyes) {
            GLES20.glScissor(eye * eyeWidth, 0, eyeWidth, viewHeight)
            GLES20.glViewport(eye * eyeWidth + Eyes.offset(eye, eyes, eyeShift).toInt(), 0, eyeWidth, viewHeight)
            drawQuad()
        }
        GLES20.glDisable(GLES20.GL_SCISSOR_TEST)
        GLES20.glViewport(0, 0, viewWidth, viewHeight)
    }

    private fun drawQuad() {
        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texture)
        val position = GLES20.glGetAttribLocation(program, "a_Position")
        val texCoord = GLES20.glGetAttribLocation(program, "a_TexCoord")
        quadCoords.position(0)
        texCoords.position(0)
        GLES20.glVertexAttribPointer(position, 2, GLES20.GL_FLOAT, false, 0, quadCoords)
        GLES20.glVertexAttribPointer(texCoord, 2, GLES20.GL_FLOAT, false, 0, texCoords)
        GLES20.glEnableVertexAttribArray(position)
        GLES20.glEnableVertexAttribArray(texCoord)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
        GLES20.glDisableVertexAttribArray(position)
        GLES20.glDisableVertexAttribArray(texCoord)
    }

    private fun floatBuffer(data: FloatArray): FloatBuffer =
        ByteBuffer.allocateDirect(data.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer().apply { put(data); position(0) }

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
