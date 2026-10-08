package nz.vbs.androidoptics

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.view.View

/**
 * The Vector's laser rangefinder display, as the plugin works it (plugin/lrf.cpp): a left and a right readout in
 * red seven-segment digits centred on the graticule's 40 mil ticks below the centre, and the aiming mark (a small square of
 * segments) at the centre while a button is being used. Drawn once per eye in Cardboard mode.
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
    private var grid = ""        // the lased grid after both buttons, MGRS without spaces
    private var fovDeg = 0f      // the graticule's optics (see setOptics)
    private var videoWidth = 0
    private var videoHeight = 0

    var eyes = 1
        set(value) {
            if (field != value) { field = value; invalidate() }
        }

    /** Pixels each eye's picture moves outward, negative inward (see Eyes) */
    var eyeShift = 0f
        set(value) {
            if (field != value) { field = value; invalidate() }
        }

    fun show(left: String, right: String, mark: Boolean, grid: String) {
        if (left == this.left && right == this.right && mark == this.mark && grid == this.grid) return
        this.left = left
        this.right = right
        this.mark = mark
        this.grid = grid
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        if (left.isEmpty() && right.isEmpty() && !mark && grid.isEmpty()) return
        val eyeWidth = width / eyes
        for (eye in 0 until eyes) {
            canvas.save()
            canvas.clipRect(eye * eyeWidth, 0, (eye + 1) * eyeWidth, height)
            canvas.translate(eye * eyeWidth + Eyes.offset(eye, eyes, eyeShift), 0f)
            drawDisplay(canvas, eyeWidth.toFloat(), height.toFloat())
            canvas.restore()
        }
    }

    /** Called with the graticule's: where the scales are, so the readouts can sit inside them */
    fun setOptics(fovDeg: Float, videoWidth: Int, videoHeight: Int) {
        if (fovDeg == this.fovDeg && videoWidth == this.videoWidth && videoHeight == this.videoHeight) return
        this.fovDeg = fovDeg
        this.videoWidth = videoWidth
        this.videoHeight = videoHeight
        invalidate()
    }

    /**
     * Each readout is centred on a 40 mil tick: left and right of centre, level with the one below it. They are
     * DIGIT_MILS tall, so they grow and shrink with the graticule as it zooms, and are moved in or shrunk only as
     * far as needed to stay inside the graticule: clear of the vertical line and the horizontal scale's ticks,
     * inside its outermost ticks (high zoom puts the 40 mil ticks off screen). Same layout as plugin/overlay.cpp.
     */
    private fun drawDisplay(canvas: Canvas, w: Float, h: Float) {
        val g = Graticule.of(w.toInt(), h.toInt(), fovDeg, videoWidth, videoHeight)
        drawGrid(canvas, w, h, if (g == null) h * DIGIT_HEIGHT else g.offset(DIGIT_MILS))
        if (g == null) { // no video yet: just below the middle
            val d = h * DIGIT_HEIGHT
            segment.strokeWidth = d * STROKE
            drawSegments(canvas, left, w / 2 - h * 0.05f - widthInDigits(left) * d, h * 0.75f, d)
            drawSegments(canvas, right, w / 2 + h * 0.05f, h * 0.75f, d)
            if (mark) drawMark(canvas, w / 2, h / 2, h / 120)
            return
        }
        val top = g.offset(5.0) // below centre, clear of the horizontal scale's long ticks
        var d = minOf(g.offset(DIGIT_MILS), (g.outerY - top) / (1 + 2 * STROKE))
        for (text in listOf(left, right)) {
            if (text.isNotEmpty()) d = minOf(d, g.outerX / (GAP + widthInDigits(text) + STROKE))
        }
        val half = d * (1 + STROKE) / 2
        val centreY = g.offset(TICK_MILS).coerceIn(top + half, maxOf(top + half, g.outerY - half))
        segment.strokeWidth = d * STROKE
        for ((text, side) in listOf(left to -1, right to 1)) {
            if (text.isEmpty()) continue
            val width = widthInDigits(text) * d
            val edge = (width + STROKE * d) / 2
            val centreX = g.offset(TICK_MILS).coerceIn(GAP * d + edge, maxOf(GAP * d + edge, g.outerX - edge))
            drawSegments(canvas, text, g.cx + side * centreX - width / 2, g.cy + centreY + d / 2, d)
        }
        if (mark) drawMark(canvas, g.cx, g.cy, h / 120)
    }

    /**
     * The lased grid (after both buttons), "60H UB 94610 34250", at the bottom centre: as tall as the readouts,
     * smaller only if it would not fit the width
     */
    private fun drawGrid(canvas: Canvas, w: Float, h: Float, readoutHeight: Float) {
        val text = gridText(grid)
        if (text.isEmpty()) return
        val d = minOf(readoutHeight, w * 0.9f / widthInDigits(text))
        segment.strokeWidth = d * STROKE
        drawSegments(canvas, text, (w - widthInDigits(text) * d) / 2, h * 0.92f, d)
    }

    /** "60HUB9461034250" -> "60H UB 94610 34250"; as it is if it is not MGRS */
    private fun gridText(mgrs: String): String {
        val m = Regex("^(\\d{1,2}[A-Z])([A-Z]{2})(\\d+)$").find(mgrs) ?: return mgrs
        val (zone, square, digits) = m.destructured
        if (digits.length % 2 != 0) return mgrs
        return "$zone $square ${digits.take(digits.length / 2)} ${digits.drop(digits.length / 2)}"
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

    // Segment characters: digits and '-' on seven segments, letters on fourteen (the middle bar split, and four
    // diagonals and two centre verticals), each a cell DIGIT_WIDTH of the height wide plus spacing.
    // Width of a readout in digit heights
    private fun widthInDigits(text: String): Float =
        if (text.isEmpty()) 0f else text.length * (DIGIT_WIDTH + SPACING) - SPACING

    private fun drawSegments(canvas: Canvas, text: String, x: Float, bottom: Float, digitH: Float) {
        val cellW = digitH * DIGIT_WIDTH
        val inset = segment.strokeWidth // Keeps the segments' ends apart, as on a real display
        var left = x
        for (c in text) {
            val on = SEGMENTS[c] ?: LETTERS[c] ?: 0
            val r = left + cellW
            val top = bottom - digitH
            val mid = bottom - digitH / 2
            val cx = left + cellW / 2
            fun seg(bit: Int, x0: Float, y0: Float, x1: Float, y1: Float) {
                if (on and bit != 0) canvas.drawLine(x0, y0, x1, y1, segment)
            }
            seg(A, left + inset, top, r - inset, top)
            seg(B, r, top + inset, r, mid - inset)
            seg(C, r, mid + inset, r, bottom - inset)
            seg(D, left + inset, bottom, r - inset, bottom)
            seg(E, left, mid + inset, left, bottom - inset)
            seg(F, left, top + inset, left, mid - inset)
            seg(G1, left + inset, mid, cx, mid)
            seg(G2, cx, mid, r - inset, mid)
            seg(H, left + inset, top + inset, cx - inset / 2, mid - inset)
            seg(I, cx, top + inset, cx, mid - inset)
            seg(J, r - inset, top + inset, cx + inset / 2, mid - inset)
            seg(K, cx - inset / 2, mid + inset, left + inset, bottom - inset)
            seg(L, cx, mid + inset, cx, bottom - inset)
            seg(M, cx + inset / 2, mid + inset, r - inset, bottom - inset)
            left += cellW + digitH * SPACING
        }
    }

    private companion object {
        const val DIGIT_MILS = 6.5       // digit height, in mils (as the graticule): about 1.8% of the view at 1.5x
        const val DIGIT_HEIGHT = 0.018f  // of the view's height, before the graticule is known
        const val TICK_MILS = 40.0       // the readouts centre on these ticks
        const val DIGIT_WIDTH = 0.55f
        const val SPACING = 0.3f
        const val STROKE = 0.13f        // segment thickness
        const val GAP = 0.5f            // from the vertical line to each readout
        // Segments: a top, b top right, c bottom right, d bottom, e bottom left, f top left, g middle (g1 left half,
        // g2 right half); for letters h, j diagonals from the top corners to the middle, i the upper centre bar,
        // k, m diagonals from the middle to the bottom corners, l the lower centre bar
        const val A = 1; const val B = 2; const val C = 4; const val D = 8; const val E = 16; const val F = 32
        const val G1 = 64; const val G2 = 128; const val G = G1 or G2
        const val H = 256; const val I = 512; const val J = 1024; const val K = 2048; const val L = 4096; const val M = 8192
        val SEGMENTS = mapOf(
            '0' to (A or B or C or D or E or F), '1' to (B or C), '2' to (A or B or G or E or D),
            '3' to (A or B or G or C or D), '4' to (F or G or B or C), '5' to (A or F or G or C or D),
            '6' to (A or F or G or E or D or C), '7' to (A or B or C), '8' to (A or B or C or D or E or F or G),
            '9' to (A or B or C or D or F or G), '-' to G, ' ' to 0)
        val LETTERS = mapOf(
            'A' to (A or B or C or E or F or G), 'B' to (A or B or C or D or I or L or G2), 'C' to (A or D or E or F),
            'D' to (A or B or C or D or I or L), 'E' to (A or D or E or F or G1), 'F' to (A or E or F or G1),
            'G' to (A or C or D or E or F or G2), 'H' to (B or C or E or F or G), 'I' to (A or D or I or L),
            'J' to (B or C or D or E), 'K' to (E or F or G1 or J or M), 'L' to (D or E or F),
            'M' to (B or C or E or F or H or J), 'N' to (B or C or E or F or H or M), 'O' to (A or B or C or D or E or F),
            'P' to (A or B or E or F or G), 'Q' to (A or B or C or D or E or F or M), 'R' to (A or B or E or F or G or M),
            'S' to (A or C or D or F or G), 'T' to (A or I or L), 'U' to (B or C or D or E or F),
            'V' to (E or F or K or J), 'W' to (B or C or E or F or K or M), 'X' to (H or J or K or M),
            'Y' to (H or J or L), 'Z' to (A or D or J or K))
    }
}
