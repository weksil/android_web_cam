package com.androidwebcam

import android.media.MediaCodec
import android.media.MediaFormat
import java.nio.ByteBuffer

/** Destination for the encoded H.264 elementary stream produced by [CameraStreamer]. */
interface VideoSink {
    /** Called once, before any frame, with the encoder output format (contains csd-0/csd-1). */
    fun onFormat(format: MediaFormat)

    /** Called for every encoded access unit. [data] is positioned at the payload. */
    fun onEncoded(data: ByteBuffer, info: MediaCodec.BufferInfo)

    fun close()
}
