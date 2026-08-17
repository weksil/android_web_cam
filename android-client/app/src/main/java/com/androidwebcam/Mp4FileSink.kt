package com.androidwebcam

import android.media.MediaCodec
import android.media.MediaFormat
import android.media.MediaMuxer
import android.util.Log
import java.io.File
import java.nio.ByteBuffer

/** Stage-1 verification sink: muxes the encoded stream into an .mp4 file. */
class Mp4FileSink(private val file: File) : VideoSink {

    private var muxer: MediaMuxer? = null
    private var track = -1
    private var frames = 0

    override fun onFormat(format: MediaFormat) {
        if (muxer != null) return
        val m = MediaMuxer(file.absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        track = m.addTrack(format)
        m.start()
        muxer = m
        Log.i(TAG, "muxer started: ${file.absolutePath}")
    }

    override fun onEncoded(data: ByteBuffer, info: MediaCodec.BufferInfo) {
        val m = muxer ?: return
        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) return
        if (info.size <= 0) return
        m.writeSampleData(track, data, info)
        frames++
    }

    override fun close() {
        muxer?.let { m ->
            runCatching { m.stop() }.onFailure { Log.w(TAG, "muxer stop: $it") }
            runCatching { m.release() }
        }
        muxer = null
        Log.i(TAG, "muxer closed, $frames frames -> ${file.length()} bytes")
    }

    val frameCount: Int get() = frames

    private companion object {
        const val TAG = "Mp4FileSink"
    }
}
