package nz.vbs.androidoptics

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.view.View

/**
 * Rounds off the corners of the picture with black, radius a third of the screen height, like looking through
 * an optic. The picture is the letterboxed video when it is showing, otherwise the whole screen.
 */
class CornerMaskView(context: Context) : View(context) {
    private val black = Paint().apply {
        color = Color.BLACK
        isAntiAlias = true
    }
    private val path = Path()
    private var videoWidth = 0
    private var videoHeight = 0

    /** 0 x 0 = no video (mask the whole screen) */
    fun setVideoSize(width: Int, height: Int) {
        if (width == videoWidth && height == videoHeight) return
        videoWidth = width
        videoHeight = height
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        var picW = width.toFloat()
        var picH = height.toFloat()
        if (videoWidth > 0 && videoHeight > 0) { // Letterboxed as VideoView draws it
            val videoAspect = videoWidth.toFloat() / videoHeight
            if (width.toFloat() / height > videoAspect) picW = height * videoAspect else picH = width / videoAspect
        }
        val left = (width - picW) / 2
        val top = (height - picH) / 2
        val radius = height / 3f
        path.reset()
        path.fillType = Path.FillType.EVEN_ODD // The screen minus the rounded picture
        path.addRect(0f, 0f, width.toFloat(), height.toFloat(), Path.Direction.CW)
        path.addRoundRect(RectF(left, top, left + picW, top + picH), radius, radius, Path.Direction.CW)
        canvas.drawPath(path, black)
    }
}
