package nz.vbs.androidoptics

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.view.View

/**
 * The Vector's laser rangefinder display, as the plugin works it (plugin/lrf.cpp): a left and a right readout in
 * red seven-segment digits below the centre of the view, and the aiming mark (a small square of segments) at the
 * centre while a button is being used. Drawn once per eye in Cardboard mode.
 */
class LrfView(context: Context) : View(context) {
    private val segment = Paint().apply {
        color = Color.rgb(255, 40, 30)
        isAntiAlias = true
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        setShadowLayer(4f, 0f, 0f, Color.BLACK) // Readable over bright ground and sky
    }

    private var left = ""
    private var right = ""
    private var mark = false

    var eyes = 1
        set(value) {
            if (field != value) { field = value; invalidate() }
        }

    fun show(left: String, right: String, mark: Boolean) {
        if (left == this.left && right == this.right && mark == this.mark) return
        this.left = left
        this.right = right
        this.mark = mark
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        if (left.isEmpty() && right.isEmpty() && !mark) return
        val eyeWidth = width / eyes
        for (eye in 0 until eyes) {
            canvas.save()
            canvas.clipRect(eye * eyeWidth, 0, (eye + 1) * eyeWidth, height)
            canvas.translate((eye * eyeWidth).toFloat(), 0f)
            drawDisplay(canvas, eyeWidth.toFloat(), height.toFloat())
            canvas.restore()
        }
    }

    private fun drawDisplay(canvas: Canvas, w: Float, h: Float) {
        val cx = w / 2
        val digitH = h * 0.045f
        val bottom = h * 0.8f
        val gap = h / 20
        segment.strokeWidth = digitH * 0.13f
        drawSegments(canvas, left, cx - gap - textWidth(left, digitH), bottom, digitH)
        drawSegments(canvas, right, cx + gap, bottom, digitH)
        if (mark) drawMark(canvas, cx, h / 2, h / 60)
    }

    /** The aiming mark: a small square of four segments, in the digits' style, centred on the view */
    private fun drawMark(canvas: Canvas, cx: Float, cy: Float, half: Float) {
        val inset = segment.strokeWidth
        val l = cx - half; val r = cx + half; val t = cy - half; val b = cy + half
        canvas.drawLine(l + inset, t, r - inset, t, segment)
        canvas.drawLine(r, t + inset, r, b - inset, segment)
        canvas.drawLine(l + inset, b, r - inset, b, segment)
        canvas.drawLine(l, t + inset, l, b - inset, segment)
    }

    // Seven-segment characters: digits, '-' and ' ', each a cell DIGIT_WIDTH of the height wide plus spacing
    private fun textWidth(text: String, digitH: Float): Float =
        if (text.isEmpty()) 0f else text.length * digitH * (DIGIT_WIDTH + SPACING) - digitH * SPACING

    private fun drawSegments(canvas: Canvas, text: String, x: Float, bottom: Float, digitH: Float) {
        val cellW = digitH * DIGIT_WIDTH
        val inset = segment.strokeWidth // Keeps the segments' ends apart, as on a real display
        var left = x
        for (c in text) {
            val on = SEGMENTS[c] ?: 0
            val r = left + cellW
            val top = bottom - digitH
            val mid = bottom - digitH / 2
            fun seg(bit: Int, x0: Float, y0: Float, x1: Float, y1: Float) {
                if (on and bit != 0) canvas.drawLine(x0, y0, x1, y1, segment)
            }
            seg(A, left + inset, top, r - inset, top)
            seg(B, r, top + inset, r, mid - inset)
            seg(C, r, mid + inset, r, bottom - inset)
            seg(D, left + inset, bottom, r - inset, bottom)
            seg(E, left, mid + inset, left, bottom - inset)
            seg(F, left, top + inset, left, mid - inset)
            seg(G, left + inset, mid, r - inset, mid)
            left += cellW + digitH * SPACING
        }
    }

    private companion object {
        const val DIGIT_WIDTH = 0.55f
        const val SPACING = 0.3f
        // Segments: a top, b top right, c bottom right, d bottom, e bottom left, f top left, g middle
        const val A = 1; const val B = 2; const val C = 4; const val D = 8; const val E = 16; const val F = 32; const val G = 64
        val SEGMENTS = mapOf(
            '0' to (A or B or C or D or E or F), '1' to (B or C), '2' to (A or B or G or E or D),
            '3' to (A or B or G or C or D), '4' to (F or G or B or C), '5' to (A or F or G or C or D),
            '6' to (A or F or G or E or D or C), '7' to (A or B or C), '8' to (A or B or C or D or E or F or G),
            '9' to (A or B or C or D or F or G), '-' to G, ' ' to 0)
    }
}
