package nz.vbs.androidoptics

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.view.View

/** A thin centre crosshair drawn over the VBS video (optics overlays live on the phone, so VBS needs no .pbo) */
class CrosshairView(context: Context) : View(context) {
    private val line = Paint().apply {
        color = Color.BLACK
        strokeWidth = 3f
        isAntiAlias = true
    }

    override fun onDraw(canvas: Canvas) {
        val cx = width / 2f
        val cy = height / 2f
        val arm = height * 0.35f
        val gap = height * 0.02f
        canvas.drawLine(cx - arm, cy, cx - gap, cy, line)
        canvas.drawLine(cx + gap, cy, cx + arm, cy, line)
        canvas.drawLine(cx, cy - arm, cx, cy - gap, line)
        canvas.drawLine(cx, cy + gap, cx, cy + arm, line)
    }
}
