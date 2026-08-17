package com.androidwebcam

import android.media.MediaCodec
import android.media.MediaFormat
import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.nio.ByteBuffer

/**
 * RFC 6184 H.264 RTP packetizer over UDP.
 *
 * Single-NAL packets when a NAL fits the MTU, FU-A fragmentation otherwise.
 * SPS/PPS are re-sent ahead of every IDR so a receiver can join at any time.
 */
class RtpSink(
    private val target: InetAddress,
    private val port: Int,
    private val ssrc: Int,
    /** true = HEVC payload format (RFC 7798), false = H.264 (RFC 6184). */
    @Volatile var hevc: Boolean = false
) : VideoSink {

    private val socket = DatagramSocket().apply {
        trafficClass = 0xB8            // DSCP EF - low delay
        sendBufferSize = 1 shl 19
    }

    /** Reported to the PC so it can punch a hole through the Windows firewall. */
    val localPort: Int get() = socket.localPort

    /** Per-frame metadata for gyro stabilization, filled in by [CameraStreamer]. */
    @Volatile var exposureNs = 0L
    @Volatile var rollingShutterSkewNs = 0L

    /**
     * MediaCodec stamps output buffers on the monotonic clock, while camera frames and
     * sensor events live on boottime - the two differ by however long the phone slept.
     * Both clocks are read back to back here, so the offset is exact to microseconds
     * and constant for the session (deep sleep cannot happen while streaming).
     */
    private val monotonicToBootNs =
        android.os.SystemClock.elapsedRealtimeNanos() - System.nanoTime()

    /**
     * Sends a non-RTP datagram down the same socket, so it reuses the firewall
     * pinhole the PC already opened for the video stream.
     */
    fun sendAux(payload: ByteArray) {
        runCatching { socket.send(DatagramPacket(payload, payload.size, target, port)) }
    }
    private val packet = DatagramPacket(ByteArray(MTU), MTU, target, port)
    private val out = ByteArray(MTU)

    private var seq = 0

    @Volatile var bytesSent = 0L; private set
    @Volatile var packetsSent = 0L; private set
    @Volatile var framesSent = 0L; private set

    /** Parameter sets, resent before every IDR so a receiver can join at any time. */
    private var parameterSets: List<ByteArray> = emptyList()

    override fun onFormat(format: MediaFormat) {
        // H.264 splits SPS and PPS across csd-0/csd-1; HEVC packs VPS+SPS+PPS into csd-0.
        val sets = ArrayList<ByteArray>(3)
        listOf("csd-0", "csd-1").forEach { key ->
            format.getByteBuffer(key)?.let { buffer ->
                val bytes = ByteArray(buffer.remaining())
                buffer.duplicate().get(bytes)
                splitNals(bytes).forEach { (offset, length) ->
                    sets.add(bytes.copyOfRange(offset, offset + length))
                }
            }
        }
        parameterSets = sets
        Log.i(TAG, "csd: ${sets.size} parameter sets ${sets.map { it.size }} -> $target:$port")
    }

    override fun onEncoded(data: ByteBuffer, info: MediaCodec.BufferInfo) {
        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) return
        if (info.size <= 0) return

        val au = ByteArray(info.size)
        data.get(au)
        val nals = splitNals(au)
        if (nals.isEmpty()) return

        val ts = ((info.presentationTimeUs * 90L) / 1000L).toInt()
        val types = nals.map { nalType(au, it.first) }
        val hasKeyFrame = types.any { if (hevc) it in 16..21 else it == NAL_IDR }
        val hasParameterSet = types.any { if (hevc) it in 32..34 else it == NAL_SPS }
        if (hasKeyFrame && !hasParameterSet) {
            parameterSets.forEach { sendNal(it, 0, it.size, ts, false) }
        }
        nals.forEachIndexed { i, r ->
            sendNal(au, r.first, r.second, ts, i == nals.lastIndex)
        }
        framesSent++

        // Ties this frame to the gyro timeline: the encoder PTS is the camera sensor
        // timestamp, which on this device shares the clock with sensor events.
        val captureNs = info.presentationTimeUs * 1000L + monotonicToBootNs
        if (framesSent <= 3) {
            Log.i(TAG, "pts=${info.presentationTimeUs * 1000}ns -> boottime $captureNs " +
                    "(offset $monotonicToBootNs)")
        }
        val meta = Control.buffer(Control.FRAME_META, 28)
            .putInt(ts)
            .putLong(captureNs)
            .putLong(exposureNs)
            .putLong(rollingShutterSkewNs)
        sendAux(meta.array())
    }

    /** No-op: the socket outlives a single streaming session (see [release]). */
    override fun close() = Unit

    /** Closes the socket; call when the link to the PC goes away. */
    fun release() {
        runCatching { socket.close() }
    }

    // ---- packetization ----

    private fun nalType(data: ByteArray, offset: Int): Int =
        if (hevc) (data[offset].toInt() shr 1) and 0x3F else data[offset].toInt() and 0x1F

    private fun sendNal(src: ByteArray, off: Int, len: Int, ts: Int, marker: Boolean) {
        if (len <= MTU - RTP_HEADER) {
            writeHeader(ts, marker)
            System.arraycopy(src, off, out, RTP_HEADER, len)
            send(RTP_HEADER + len)
            return
        }
        if (hevc) sendFragmentedHevc(src, off, len, ts, marker)
        else sendFragmentedAvc(src, off, len, ts, marker)
    }

    /** RFC 6184 FU-A: one-byte NAL header replaced by indicator + FU header. */
    private fun sendFragmentedAvc(src: ByteArray, off: Int, len: Int, ts: Int, marker: Boolean) {
        val nalHeader = src[off].toInt() and 0xFF
        val indicator = ((nalHeader and 0xE0) or FU_A).toByte()
        val type = nalHeader and 0x1F
        val max = MTU - RTP_HEADER - 2

        var pos = off + 1
        var left = len - 1
        var first = true
        while (left > 0) {
            val chunk = minOf(max, left)
            val last = chunk == left
            writeHeader(ts, marker && last)
            out[RTP_HEADER] = indicator
            out[RTP_HEADER + 1] = (type or
                    (if (first) 0x80 else 0) or
                    (if (last) 0x40 else 0)).toByte()
            System.arraycopy(src, pos, out, RTP_HEADER + 2, chunk)
            send(RTP_HEADER + 2 + chunk)
            pos += chunk
            left -= chunk
            first = false
        }
    }

    /**
     * RFC 7798 fragmentation unit. HEVC NAL headers are two bytes, and the payload header
     * keeps the original layer/temporal id with the type replaced by 49.
     */
    private fun sendFragmentedHevc(src: ByteArray, off: Int, len: Int, ts: Int, marker: Boolean) {
        val type = (src[off].toInt() shr 1) and 0x3F
        val layerAndTid = ((src[off].toInt() and 0x01) shl 8) or (src[off + 1].toInt() and 0xFF)
        val header0 = ((49 shl 1) or (layerAndTid shr 8)).toByte()
        val header1 = (layerAndTid and 0xFF).toByte()
        val max = MTU - RTP_HEADER - 3

        var pos = off + 2
        var left = len - 2
        var first = true
        while (left > 0) {
            val chunk = minOf(max, left)
            val last = chunk == left
            writeHeader(ts, marker && last)
            out[RTP_HEADER] = header0
            out[RTP_HEADER + 1] = header1
            out[RTP_HEADER + 2] = (type or
                    (if (first) 0x80 else 0) or
                    (if (last) 0x40 else 0)).toByte()
            System.arraycopy(src, pos, out, RTP_HEADER + 3, chunk)
            send(RTP_HEADER + 3 + chunk)
            pos += chunk
            left -= chunk
            first = false
        }
    }

    private fun writeHeader(ts: Int, marker: Boolean) {
        out[0] = 0x80.toByte()                                      // V=2, no padding/ext/CSRC
        out[1] = (PAYLOAD_TYPE or (if (marker) 0x80 else 0)).toByte()
        out[2] = (seq ushr 8).toByte()
        out[3] = seq.toByte()
        seq = (seq + 1) and 0xFFFF
        writeInt(4, ts)
        writeInt(8, ssrc)
    }

    private fun writeInt(at: Int, v: Int) {
        out[at] = (v ushr 24).toByte()
        out[at + 1] = (v ushr 16).toByte()
        out[at + 2] = (v ushr 8).toByte()
        out[at + 3] = v.toByte()
    }

    private fun send(len: Int) {
        packet.setData(out, 0, len)
        try {
            socket.send(packet)
            packetsSent++
            bytesSent += len
        } catch (t: Throwable) {
            Log.w(TAG, "send: $t")
        }
    }

    // ---- Annex-B helpers ----

    /** Returns (offset, length) of every NAL payload, start codes excluded. */
    private fun splitNals(b: ByteArray): List<Pair<Int, Int>> {
        val scStart = ArrayList<Int>(4)   // index of the start code
        val nalStart = ArrayList<Int>(4)  // index of the NAL header byte
        var i = 0
        while (i + 2 < b.size) {
            if (b[i].toInt() == 0 && b[i + 1].toInt() == 0) {
                if (b[i + 2].toInt() == 1) {
                    scStart.add(i); nalStart.add(i + 3); i += 3; continue
                }
                if (i + 3 < b.size && b[i + 2].toInt() == 0 && b[i + 3].toInt() == 1) {
                    scStart.add(i); nalStart.add(i + 4); i += 4; continue
                }
            }
            i++
        }
        return nalStart.mapIndexed { idx, start ->
            val end = if (idx + 1 < scStart.size) scStart[idx + 1] else b.size
            start to (end - start)
        }.filter { it.second > 0 }
    }

    companion object {
        const val MTU = 1400
        const val RTP_HEADER = 12
        const val PAYLOAD_TYPE = 96
        private const val FU_A = 28
        private const val NAL_IDR = 5
        private const val NAL_SPS = 7
        private const val TAG = "RtpSink"
    }
}
