package com.androidwebcam

import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * AndroidWebCam control protocol — plain binary over UDP, big-endian.
 *
 *   header: "AWC1" (4 bytes) + type (1 byte) + payload
 *
 * PC -> phone (phone listens on [CONTROL_PORT] = 45001):
 *   0x01 HELLO         u16 rtpPort                 link up, allocate the RTP socket
 *   0x03 KEEPALIVE     -                           must arrive at least every 5 s
 *   0x04 REQUEST_IDR   -                           force a key frame
 *   0x05 SET_BITRATE   u32 bitsPerSecond
 *   0x06 BYE           -                           drop the link
 *   0x07 STREAM_START  -                           open the camera and start sending
 *   0x08 STREAM_STOP   -                           release the camera, keep the link
 *   0x10 DISCOVER      -                           broadcast probe
 *
 * HELLO only establishes the link; the camera stays off until STREAM_START, which
 * the PC sends when an application actually opens the virtual camera.
 *
 * phone -> PC:
 *   0x02 HELLO_ACK     u16 w, u16 h, u8 fps, u32 bitrate, u32 ssrc, u16 rtpSourcePort,
 *                      u8 codec (0 = H.264, 1 = HEVC)
 *   0x11 FOUND         u16 controlPort, u8 nameLen, name (UTF-8)
 *
 * Video: RTP/UDP, payload type 96, 90 kHz clock, RFC 6184 (single NAL + FU-A).
 * The PC listens for RTP on the port it advertised in HELLO (default 45000).
 *
 * Firewall: Windows blocks unsolicited inbound UDP. The PC therefore sends a
 * PUNCH datagram (0x12, no payload) from its RTP socket to phone:rtpSourcePort
 * right after HELLO_ACK and once per keepalive; that opens the stateful UDP
 * mapping so the RTP stream is accepted without an administrative rule.
 */
object Control {
    const val CONTROL_PORT = 45001
    const val DEFAULT_RTP_PORT = 45000
    const val KEEPALIVE_TIMEOUT_MS = 5000L

    val MAGIC = byteArrayOf(0x41, 0x57, 0x43, 0x31) // "AWC1"

    const val HELLO = 0x01
    const val HELLO_ACK = 0x02
    const val KEEPALIVE = 0x03
    const val REQUEST_IDR = 0x04
    const val SET_BITRATE = 0x05
    const val BYE = 0x06
    const val STREAM_START = 0x07
    const val STREAM_STOP = 0x08
    const val SET_EIS = 0x09     // u8: 1 = phone stabilizes, 0 = PC stabilizes from gyro
    const val SET_EXPOSURE = 0x0A // i64 exposureNs, i32 iso; exposureNs 0 = back to auto
    // u8 idLen, id, u16 w, u16 h, u8 fps, u32 bitrate, u8 codec: the PC owns the settings
    const val SET_CONFIG = 0x0B
    const val DISCOVER = 0x10
    const val FOUND = 0x11
    const val PUNCH = 0x12

    // Telemetry for gyro stabilization, sent down the RTP socket (reuses its pinhole).
    const val GYRO = 0x20        // u8 count, count * { i64 tNs, f32 wx, wy, wz }
    const val CAM_INFO = 0x21    // f32 fx, fy, cx, cy, u16 w, h, i32 orientation, u8 facing
    const val FRAME_META = 0x22  // u32 rtpTs, i64 sensorNs, i64 exposureNs, i64 skewNs
    const val SENSOR_LIMITS = 0x23 // i32 isoMin, isoMax, i64 exposureMinNs, exposureMaxNs
    const val GRAVITY = 0x24     // f32 gx, gy, gz in device axes, m/s^2
    // u8 cameraCount, per camera { u8 idLen, id, u8 labelLen, label, u8 sizes, per size u16 w,h },
    // then u8 fpsCount, fps values: everything the PC needs to offer a choice.
    const val CAPABILITIES = 0x26

    fun isValid(data: ByteArray, len: Int): Boolean =
        len >= 5 && data[0] == MAGIC[0] && data[1] == MAGIC[1] &&
                data[2] == MAGIC[2] && data[3] == MAGIC[3]

    fun buffer(type: Int, payload: Int): ByteBuffer =
        ByteBuffer.allocate(5 + payload).order(ByteOrder.BIG_ENDIAN).apply {
            put(MAGIC); put(type.toByte())
        }
}

data class StreamInfo(
    val width: Int,
    val height: Int,
    val fps: Int,
    val bitrate: Int,
    val ssrc: Int,
    val rtpSourcePort: Int,
    val hevc: Boolean
)

/**
 * Listens for control datagrams and drives the streaming lifecycle.
 * Callbacks run on the control thread.
 */
class ControlServer(
    private val deviceName: String,
    private val onLink: (InetAddress, Int) -> StreamInfo?,
    private val onUnlink: () -> Unit,
    private val onStreamStart: () -> Unit,
    private val onStreamStop: () -> Unit,
    private val onIdr: () -> Unit,
    private val onBitrate: (Int) -> Unit,
    private val onEis: (Boolean) -> Unit,
    private val onExposure: (Long, Int) -> Unit,
    private val onConfig: (String, Int, Int, Int, Int, Boolean) -> Unit,
    private val onPeer: (String?) -> Unit
) {
    private var socket: DatagramSocket? = null
    private var thread: Thread? = null
    @Volatile private var running = false

    private var peer: InetAddress? = null
    private var peerPort = 0
    private var lastSeen = 0L

    fun start() {
        if (running) return
        running = true
        val s = DatagramSocket(null).apply {
            reuseAddress = true
            broadcast = true
            soTimeout = 1000
            bind(java.net.InetSocketAddress(Control.CONTROL_PORT))
        }
        socket = s
        thread = Thread({ loop(s) }, "control").apply { start() }
    }

    fun stop() {
        running = false
        runCatching { socket?.close() }
        thread?.join(1500)
        thread = null
        socket = null
        dropPeer()
    }

    private fun loop(s: DatagramSocket) {
        val buf = ByteArray(512)
        val packet = DatagramPacket(buf, buf.size)
        while (running) {
            try {
                packet.setData(buf, 0, buf.size)
                s.receive(packet)
                handle(s, packet)
            } catch (_: java.net.SocketTimeoutException) {
            } catch (t: Throwable) {
                if (running) Log.w(TAG, "control: $t")
            }
            if (peer != null && System.currentTimeMillis() - lastSeen > Control.KEEPALIVE_TIMEOUT_MS) {
                Log.i(TAG, "keepalive timeout, dropping peer")
                dropPeer()
            }
        }
    }

    private fun handle(s: DatagramSocket, p: DatagramPacket) {
        val data = p.data
        if (!Control.isValid(data, p.length)) return
        val body = ByteBuffer.wrap(data, 5, p.length - 5).order(ByteOrder.BIG_ENDIAN)

        when (data[4].toInt() and 0xFF) {
            Control.DISCOVER -> {
                val name = deviceName.take(48).toByteArray(Charsets.UTF_8)
                val out = Control.buffer(Control.FOUND, 3 + name.size)
                    .putShort(Control.CONTROL_PORT.toShort())
                    .put(name.size.toByte())
                    .put(name)
                send(s, out.array(), p.address, p.port)
            }

            Control.HELLO -> {
                val rtpPort = if (body.remaining() >= 2) body.short.toInt() and 0xFFFF
                             else Control.DEFAULT_RTP_PORT
                if (peer != null) onUnlink()
                peer = p.address
                peerPort = rtpPort
                lastSeen = System.currentTimeMillis()
                val info = onLink(p.address, rtpPort)
                if (info == null) { dropPeer(); return }
                onPeer("${p.address.hostAddress}:$rtpPort")
                val out = Control.buffer(Control.HELLO_ACK, 16)
                    .putShort(info.width.toShort())
                    .putShort(info.height.toShort())
                    .put(info.fps.toByte())
                    .putInt(info.bitrate)
                    .putInt(info.ssrc)
                    .putShort(info.rtpSourcePort.toShort())
                    .put(if (info.hevc) 1.toByte() else 0.toByte())
                send(s, out.array(), p.address, p.port)
            }

            Control.KEEPALIVE -> if (p.address == peer) lastSeen = System.currentTimeMillis()
            Control.REQUEST_IDR -> if (p.address == peer) onIdr()
            Control.SET_BITRATE -> if (p.address == peer && body.remaining() >= 4) onBitrate(body.int)
            Control.SET_EIS -> if (p.address == peer && body.remaining() >= 1) onEis(body.get() != 0.toByte())
            Control.SET_EXPOSURE -> if (p.address == peer && body.remaining() >= 12)
                onExposure(body.long, body.int)

            Control.SET_CONFIG -> if (p.address == peer && body.remaining() >= 11) {
                val idLength = body.get().toInt() and 0xFF
                if (body.remaining() >= idLength + 10) {
                    val id = ByteArray(idLength).also { body.get(it) }.toString(Charsets.UTF_8)
                    val width = body.short.toInt() and 0xFFFF
                    val height = body.short.toInt() and 0xFFFF
                    val fps = body.get().toInt() and 0xFF
                    val bitrate = body.int
                    val hevc = body.get() != 0.toByte()
                    onConfig(id, width, height, fps, bitrate, hevc)
                }
            }
            Control.STREAM_START -> if (p.address == peer) { lastSeen = System.currentTimeMillis(); onStreamStart() }
            Control.STREAM_STOP -> if (p.address == peer) { lastSeen = System.currentTimeMillis(); onStreamStop() }
            Control.BYE -> if (p.address == peer) dropPeer()
        }
    }

    private fun dropPeer() {
        if (peer != null) {
            peer = null
            onUnlink()
            onPeer(null)
        }
    }

    private fun send(s: DatagramSocket, bytes: ByteArray, address: InetAddress, port: Int) {
        runCatching { s.send(DatagramPacket(bytes, bytes.size, address, port)) }
            .onFailure { Log.w(TAG, "reply: $it") }
    }

    private companion object {
        const val TAG = "ControlServer"
    }
}
