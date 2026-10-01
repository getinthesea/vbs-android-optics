package nz.vbs.androidoptics

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.view.View
import kotlin.math.atan
import kotlin.math.floor
import kotlin.math.tan

/**
 * Binocular graticule drawn over the VBS video (the app draws the optics overlay, so VBS needs no .pbo):
 * scales of ticks every 10 NATO mils (6400 to a circle), alternately 5 and 10 mils long, with a long tick at
 * the centre. They run 60 mils left, right, up and down, each scale joined by a line, and the 20, 40 and 60 mil
 * ticks left and right are labelled above, in plain text a quarter as tall as a long tick.
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
    private val label = Paint().apply {
        color = Color.BLACK
        isAntiAlias = true
        textAlign = Paint.Align.CENTER
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

    /** 1, or 2 for Cardboard: a graticule centred in each half of the screen */
    var eyes = 1
        set(value) {
            if (field != value) { field = value; invalidate() }
        }

    override fun onDraw(canvas: Canvas) {
        if (fovDeg <= 0f || videoWidth == 0 || videoHeight == 0) return
        val eyeWidth = width / eyes
        for (eye in 0 until eyes) {
            canvas.save()
            canvas.clipRect(eye * eyeWidth, 0, (eye + 1) * eyeWidth, height)
            canvas.translate((eye * eyeWidth).toFloat(), 0f)
            drawGraticule(canvas, eyeWidth, height)
            canvas.restore()
        }
    }

    private fun drawGraticule(canvas: Canvas, width: Int, height: Int) {
        // The video fills its area keeping its shape (as VideoView draws it), cropped at two edges
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

        // Lines joining the ticks: across from the outermost left tick to the outermost right, and from the top
        // tick to the bottom one
        val outerX = offsetPx(floor(minOf(60.0, edgeX) / 10) * 10)
        val outerY = offsetPx(floor(minOf(60.0, edgeY) / 10) * 10)
        canvas.drawLine(cx - outerX, cy, cx + outerX, cy, line)
        canvas.drawLine(cx, cy - outerY, cx, cy + outerY, line)

        // "20", "40" and "60" above those ticks either side, a quarter as tall as a long tick (10 mils), a little
        // clear of the tick's top
        val longHalf = offsetPx(5.0)
        val bounds = android.graphics.Rect()
        label.textSize = 100f
        label.getTextBounds("60", 0, 2, bounds)
        label.textSize = 100f * (longHalf / 2) / bounds.height() // cap height = a quarter of a long tick
        val baseline = cy - longHalf - longHalf / 3
        for (mils in listOf(20, 40, 60)) {
            if (mils > edgeX) break
            val d = offsetPx(mils.toDouble())
            for (x in listOf(cx - d, cx + d)) canvas.drawText(mils.toString(), x, baseline, label)
        }

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
            // Vertical scale: level ticks above and below centre
            if (n > 0 && mils <= edgeY) {
                canvas.drawLine(cx - half, cy - d, cx + half, cy - d, line)
                canvas.drawLine(cx - half, cy + d, cx + half, cy + d, line)
            }
        }
    }
}
