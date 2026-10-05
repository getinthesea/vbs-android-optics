package nz.vbs.androidoptics

import kotlin.math.atan
import kotlin.math.floor
import kotlin.math.tan

/**
 * Where the binocular graticule falls in one eye's area (the whole view, or half of it in Cardboard), shared by
 * the graticule (CrosshairView) and the rangefinder readouts (LrfView) so they line up.
 *
 * The video fills the area keeping its shape (as VideoView draws it), cropped at two edges, and is fovDeg across
 * its full width. Directions are placed through the same perspective VBS renders with, so they stay true at every
 * zoom: a direction [mils] off-centre lands tan(angle) / tan(fov / 2) of the picture's half-width from the centre.
 */
class Graticule private constructor(width: Int, height: Int, fovDeg: Float, videoAspect: Float) {
    val cx = width / 2f
    val cy = height / 2f
    private val scale: Double // pixels per unit of tangent, the same both ways (square pixels)

    /** Mils from the centre to the edges of the area */
    val edgeX: Double
    val edgeY: Double

    /** Pixels from the centre to the outermost ticks drawn: 60 mils, or the last whole 10 inside the edge */
    val outerX: Float
    val outerY: Float

    init {
        val viewAspect = width.toFloat() / height
        val picW = if (viewAspect > videoAspect) width.toFloat() else height * videoAspect
        scale = (picW / 2) / tan(Math.toRadians(fovDeg / 2.0))
        edgeX = atan(width / 2.0 / scale) / RAD_PER_MIL
        edgeY = atan(height / 2.0 / scale) / RAD_PER_MIL
        outerX = offset(floor(minOf(MAX_MILS, edgeX) / 10) * 10)
        outerY = offset(floor(minOf(MAX_MILS, edgeY) / 10) * 10)
    }

    /** Pixels from the centre to a direction [mils] off it */
    fun offset(mils: Double): Float = (tan(mils * RAD_PER_MIL) * scale).toFloat()

    companion object {
        const val MAX_MILS = 60.0
        const val RAD_PER_MIL = 2 * Math.PI / 6400

        /** null until the field of view and the video's size are known */
        fun of(width: Int, height: Int, fovDeg: Float, videoWidth: Int, videoHeight: Int): Graticule? =
            if (width <= 0 || height <= 0 || fovDeg <= 0f || videoWidth <= 0 || videoHeight <= 0) null
            else Graticule(width, height, fovDeg, videoWidth.toFloat() / videoHeight)
    }
}
