package nz.vbs.androidoptics

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.view.View

/**
 * Rounds off the corners of the screen with black, radius a third of the screen height, like looking through
 * an optic. The video fills the screen, so the mask always follows the screen's edges.
 */
class CornerMaskView(context: Context) : View(context) {
    private val black = Paint().apply {
        color = Color.BLACK
        isAntiAlias = true
    }
    private val path = Path()

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        val radius = h / 3f
        path.reset()
        path.fillType = Path.FillType.EVEN_ODD // The screen minus the rounded picture
        path.addRect(0f, 0f, w, h, Path.Direction.CW)
        path.addRoundRect(RectF(0f, 0f, w, h), radius, radius, Path.Direction.CW)
        canvas.drawPath(path, black)
    }
}
