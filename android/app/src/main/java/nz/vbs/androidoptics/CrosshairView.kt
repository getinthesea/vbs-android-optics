package nz.vbs.androidoptics

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.view.View
import kotlin.math.atan
import kotlin.math.tan

/**
 * Binocular graticule drawn over the VBS video (the app draws the optics overlay, so VBS needs no .pbo):
 * a vertical centre line, and along the horizontal centre a row of ticks every 10 NATO mils (6400 to a circle),
 * alternately 5 and 10 mils tall, with a tall tick in the centre.
 *
 * Ticks are placed by angle through the same perspective VBS renders with, so they stay true at every zoom:
 * a direction [angle] off-centre lands tan(angle) / tan(fov / 2) of the half-width from the centre.
 */
class CrosshairView(context: Context) : View(context) {
    private val line = Paint().apply {
        color = Color.BLACK
        strokeWidth = 3f
        isAntiAlias = true
    }

    private var fovDeg = 0f      // horizontal field of view of the video (what the plugin set in VBS)
    private var videoWidth = 0
    private var videoHeight = 0

    /** Called when the zoom or the video size changes */
    fun setOptics(fovDeg: Float, videoWidth: Int, videoHeight: Int) {
        if (fovDeg == this.fovDeg && videoWidth == this.videoWidth && videoHeight == this.videoHeight) return
        this.fovDeg = fovDeg
        this.videoWidth = videoWidth
        this.videoHeight = videoHeight
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        if (fovDeg <= 0f || videoWidth == 0 || videoHeight == 0) return

        // The video is letterboxed to its shape (as VideoView draws it): work in that rectangle
        val videoAspect = videoWidth.toFloat() / videoHeight
        val viewAspect = width.toFloat() / height
        val picW = if (viewAspect > videoAspect) height * videoAspect else width.toFloat()
        val picH = if (viewAspect > videoAspect) height.toFloat() else width / videoAspect
        val cx = width / 2f
        val cy = height / 2f

        // Pixels per unit of tangent: the same scale horizontally and vertically (square pixels)
        val tanHalfFov = tan(Math.toRadians(fovDeg / 2.0))
        val scale = (picW / 2) / tanHalfFov
        val radPerMil = 2 * Math.PI / 6400
        fun offsetPx(mils: Double) = (tan(mils * radPerMil) * scale).toFloat()

        // Vertical centre line
        canvas.drawLine(cx, cy - picH / 2, cx, cy + picH / 2, line)

        // Ticks every 10 mils out to the edge of the picture, alternately 5 and 10 mils tall, centred on the line
        val edgeMils = atan(tanHalfFov) / radPerMil
        var n = 0
        while (n * 10.0 <= edgeMils) {
            val half = offsetPx(if (n % 2 == 0) 5.0 else 2.5)
            val dx = offsetPx(n * 10.0)
            for (x in if (n == 0) listOf(cx) else listOf(cx - dx, cx + dx)) {
                canvas.drawLine(x, cy - half, x, cy + half, line)
            }
            n++
        }
    }
}
