package com.androidwebcam

import android.content.Context
import android.widget.FrameLayout

/** Letterboxes its children to [aspect] (width / height). */
class AspectFrameLayout(context: Context) : FrameLayout(context) {

    var aspect = 16f / 9f
        set(value) {
            field = value
            requestLayout()
        }

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        val w = MeasureSpec.getSize(widthSpec)
        val h = MeasureSpec.getSize(heightSpec)
        if (w == 0 || h == 0) {
            super.onMeasure(widthSpec, heightSpec)
            return
        }
        var cw = w
        var ch = (w / aspect).toInt()
        if (ch > h) {
            ch = h
            cw = (h * aspect).toInt()
        }
        super.onMeasure(
            MeasureSpec.makeMeasureSpec(cw, MeasureSpec.EXACTLY),
            MeasureSpec.makeMeasureSpec(ch, MeasureSpec.EXACTLY)
        )
    }
}
