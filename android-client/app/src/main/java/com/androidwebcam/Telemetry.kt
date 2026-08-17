package com.androidwebcam

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Handler
import android.os.HandlerThread
import android.util.Log

/**
 * Streams gyroscope samples to the PC so it can stabilize the picture itself.
 *
 * Sample timestamps come from the same clock as the camera frame timestamps
 * (SENSOR_INFO_TIMESTAMP_SOURCE == REALTIME), which is what makes the alignment
 * between rotation and frames possible at all.
 */
class Telemetry(context: Context, private val send: (ByteArray) -> Unit) : SensorEventListener {

    private val sensors = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
    private val gyro: Sensor? = sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE)

    /**
     * Horizon levelling needs to know where down is, and a gyroscope cannot tell:
     * integrating angular velocity gives orientation relative to wherever it started,
     * plus drift. Gravity is the absolute reference.
     */
    private val gravity: Sensor? = sensors.getDefaultSensor(Sensor.TYPE_GRAVITY)
    private val thread = HandlerThread("gyro")

    private val batch = ArrayList<FloatArray>(BATCH)
    private val stamps = ArrayList<Long>(BATCH)

    @Volatile var samplesSent = 0L; private set

    fun start(): Boolean {
        if (gyro == null) { Log.w(TAG, "no gyroscope"); return false }
        if (started) return true
        started = true
        thread.start()
        val handler = Handler(thread.looper)
        val ok = sensors.registerListener(this, gyro, RATE_US, handler)
        Log.i(TAG, "gyro ${gyro.name} @${1_000_000 / RATE_US}Hz registered=$ok")
        if (gravity != null) {
            sensors.registerListener(this, gravity, GRAVITY_RATE_US, handler)
            Log.i(TAG, "gravity ${gravity.name} @${1_000_000 / GRAVITY_RATE_US}Hz")
        } else {
            Log.w(TAG, "no gravity sensor: horizon levelling unavailable")
        }
        return ok
    }

    private var started = false

    fun stop() {
        sensors.unregisterListener(this)
        flush()
        if (started) thread.quitSafely()
        started = false
    }

    private var cameraInfo: ByteArray? = null
    private var lastInfoSent = 0L

    fun sendCameraInfo(fx: Float, fy: Float, cx: Float, cy: Float, width: Int, height: Int,
                       orientation: Int, facing: Int) {
        val b = Control.buffer(Control.CAM_INFO, 25)
            .putFloat(fx).putFloat(fy).putFloat(cx).putFloat(cy)
            .putShort(width.toShort()).putShort(height.toShort())
            .putInt(orientation).put(facing.toByte())
        cameraInfo = b.array()
        resendCameraInfo()
        Log.i(TAG, "cam info fx=$fx fy=$fy cx=$cx cy=$cy ${width}x$height orient=$orientation")
    }

    private var sensorLimits: ByteArray? = null
    private var capabilities: ByteArray? = null

    /**
     * Cameras with their 16:9 modes. Frame rates are per resolution on purpose: 120 fps
     * only exists up to 1080p and 240 fps only up to 720p, so a single global list would
     * offer the PC settings that cannot be applied.
     */
    fun sendCapabilities(cameras: List<Cameras.Info>) {
        val bodySize = cameras.sumOf { camera ->
            1 + camera.id.toByteArray().size + 1 + camera.label.toByteArray(Charsets.UTF_8).size +
                    1 + camera.modes.sumOf { 5 + it.fps.size }
        } + 1
        val b = Control.buffer(Control.CAPABILITIES, bodySize).put(cameras.size.toByte())
        cameras.forEach { camera ->
            val id = camera.id.toByteArray()
            val label = camera.label.toByteArray(Charsets.UTF_8)
            b.put(id.size.toByte()).put(id).put(label.size.toByte()).put(label)
            b.put(camera.modes.size.toByte())
            camera.modes.forEach { mode ->
                b.putShort(mode.size.width.toShort()).putShort(mode.size.height.toShort())
                b.put(mode.fps.size.toByte())
                mode.fps.forEach { b.put(it.toByte()) }
            }
        }
        capabilities = b.array()
        // Sent before any periodic telemetry exists, so repeat it: one lost datagram would
        // otherwise leave the PC menus empty until a stream starts.
        repeat(3) { send(b.array()) }
        Log.i(TAG, "capabilities: " + cameras.joinToString { c ->
            "${c.label}[" + c.modes.joinToString(";") { "${it.size}=${it.fps}" } + "]"
        })
    }

    /** Exposure and ISO limits, so the PC can drive them without guessing. */
    fun sendSensorLimits(isoMin: Int, isoMax: Int, exposureMinNs: Long, exposureMaxNs: Long) {
        sensorLimits = Control.buffer(Control.SENSOR_LIMITS, 24)
            .putInt(isoMin).putInt(isoMax).putLong(exposureMinNs).putLong(exposureMaxNs)
            .array()
        Log.i(TAG, "sensor limits ISO $isoMin..$isoMax exposure ${exposureMinNs / 1000}..${exposureMaxNs / 1000}us")
    }

    /** UDP loses datagrams, so the one-shot geometry message is repeated. */
    private fun resendCameraInfo() {
        val info = cameraInfo ?: return
        val now = System.currentTimeMillis()
        if (now - lastInfoSent < 2000) return
        lastInfoSent = now
        send(info)
        sensorLimits?.let { send(it) }
        capabilities?.let { send(it) }
    }

    override fun onSensorChanged(event: SensorEvent) {
        if (event.sensor.type == Sensor.TYPE_GRAVITY) {
            val b = Control.buffer(Control.GRAVITY, 12)
                .putFloat(event.values[0]).putFloat(event.values[1]).putFloat(event.values[2])
            send(b.array())
            return
        }
        synchronized(batch) {
            batch.add(event.values.copyOf(3))
            stamps.add(event.timestamp)
            if (batch.size >= BATCH) flushLocked()
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

    private fun flush() = synchronized(batch) { flushLocked() }

    private fun flushLocked() {
        if (batch.isEmpty()) return
        val count = batch.size
        val b = Control.buffer(Control.GYRO, 1 + count * 20).put(count.toByte())
        for (i in 0 until count) {
            b.putLong(stamps[i])
            b.putFloat(batch[i][0]).putFloat(batch[i][1]).putFloat(batch[i][2])
        }
        send(b.array())
        samplesSent += count
        batch.clear()
        stamps.clear()
        resendCameraInfo()
    }

    private companion object {
        const val TAG = "Telemetry"
        const val RATE_US = 5000          // 200 Hz, the fastest this sensor reports
        const val GRAVITY_RATE_US = 40000 // 25 Hz is plenty for an absolute reference
        const val BATCH = 10              // ~20 datagrams per second
    }
}
