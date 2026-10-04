package nz.vbs.androidoptics

/**
 * Cardboard: where each eye's picture sits. Centred in its half of the screen, the two pictures are half the
 * screen's width apart (about 75 mm on a 6.4" phone), but a viewer's lenses are usually 60-65 mm apart, so the
 * eyes have to diverge to fuse them. Setting the lens spacing moves both pictures (video, mask, graticule and
 * readouts) sideways so their centres sit behind the lenses.
 */
object Eyes {
    const val DEFAULT_MM = 55f
    const val MIN_MM = 40f
    const val MAX_MM = 90f

    /** How far [eye] (0 left, 1 right) moves sideways: [shift] outward, so the left eye's negative */
    fun offset(eye: Int, eyes: Int, shift: Float): Float = when {
        eyes < 2 -> 0f
        eye == 0 -> -shift
        else -> shift
    }

    /** Pixels each picture moves outward (negative inward) to put their centres [lensMm] apart */
    fun shift(screenWidthPx: Int, pxPerMm: Float, lensMm: Float): Float {
        val half = screenWidthPx / 2f
        return (lensMm * pxPerMm - half) / 2
    }
}
