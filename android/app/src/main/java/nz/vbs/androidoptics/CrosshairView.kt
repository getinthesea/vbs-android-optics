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
 * scales of ticks every 10 NATO mils (6400 to a circle), alternately 5 and 10 mils long, with a long tick at
 * the centre. Horizontally they run 60 mils left and right; vertically 60 mils up from the centre, with the
 * lower half left clear.
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

        // The video fills the screen keeping its shape (as VideoView draws it), cropped at two edges
        val videoAspect = videoWidth.toFloat() / videoHeight
        val viewAspect = width.toFloat() / height
        val picW = if (viewAspect > videoAspect) width.toFloat() else height * videoAspect
        val cx = width / 2f
        val cy = height / 2f

        // Pixels per unit of tangent: the same scale horizontally and vertically (square pixels)
        val tanHalfFov = tan(Math.toRadians(fovDeg / 2.0))
        val scale = (picW / 2) / tanHalfFov
        val radPerMil = 2 * Math.PI / 6400
        fun offsetPx(mils: Double) = (tan(mils * radPerMil) * scale).toFloat()

        // Out to 60 mils, or the edge of the screen if that comes first (high zoom)
        val edgeX = atan(width / 2.0 / scale) / radPerMil
        val edgeY = atan(height / 2.0 / scale) / radPerMil
        for (n in 0..6) {
            val mils = n * 10.0
            val half = offsetPx(if (n % 2 == 0) 5.0 else 2.5) // long ticks 10 mils, short 5, centred on the axis
            val d = offsetPx(mils)
            // Horizontal scale: upright ticks left and right of centre
            if (mils <= edgeX) {
                for (x in if (n == 0) listOf(cx) else listOf(cx - d, cx + d)) {
                    canvas.drawLine(x, cy - half, x, cy + half, line)
                }
            }
            // Vertical scale: level ticks going up from centre only
            if (n > 0 && mils <= edgeY) canvas.drawLine(cx - half, cy - d, cx + half, cy - d, line)
        }
    }
}
