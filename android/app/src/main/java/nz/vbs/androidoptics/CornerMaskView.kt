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
 * an optic. The video fills the screen, so the mask always follows the screen's edges. In Cardboard mode each
 * half of the screen (one per eye) is rounded off the same way.
 */
class CornerMaskView(context: Context) : View(context) {
    private val black = Paint().apply {
        color = Color.BLACK
        isAntiAlias = true
    }
    private val path = Path()

    var eyes = 1
        set(value) {
            if (field != value) { field = value; invalidate() }
        }

    override fun onDraw(canvas: Canvas) {
        val eyeW = width.toFloat() / eyes
        val h = height.toFloat()
        val radius = h / 3f
        path.reset()
        path.fillType = Path.FillType.EVEN_ODD // The screen minus the rounded pictures
        path.addRect(0f, 0f, width.toFloat(), h, Path.Direction.CW)
        for (eye in 0 until eyes) {
            path.addRoundRect(RectF(eye * eyeW, 0f, (eye + 1) * eyeW, h), radius, radius, Path.Direction.CW)
        }
        canvas.drawPath(path, black)
    }
}
