package com.androidwebcam

import android.annotation.SuppressLint
import android.content.Context
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.hardware.camera2.params.MeteringRectangle
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface
import java.util.concurrent.Executor

/**
 * Camera2 -> MediaCodec (hardware H.264) pipeline.
 *
 * The camera writes straight into the encoder's input Surface, so no frame ever
 * round-trips through Java memory. Encoded access units are pushed to [sink].
 */
class CameraStreamer(
    context: Context,
    private val cameraId: String,
    private val size: Size,
    private val fps: Int,
    bitrate: Int,
    private val sink: VideoSink,
    private val previewSurface: Surface?,
    private val onError: (String) -> Unit,
    private val onAuto: (String) -> Unit = {},
    /** false when the PC stabilizes from gyro data: on-device EIS would fight it. */
    private val stabilizeOnDevice: Boolean = true
) {
    private val rtpSink = sink as? RtpSink
    private val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager

    private val thread = HandlerThread("cam").apply { start() }
    private val handler = Handler(thread.looper)
    private val executor = Executor { handler.post(it) }

    private var encoder: MediaCodec? = null
    private var inputSurface: Surface? = null
    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var drainThread: Thread? = null

    @Volatile private var running = false
    @Volatile var currentBitrate = bitrate
        private set

    @SuppressLint("MissingPermission")
    fun start() {
        if (running) return
        running = true
        try {
            startEncoder()
        } catch (t: Throwable) {
            fail("encoder init failed: $t")
            return
        }
        try {
            manager.openCamera(cameraId, executor, deviceCallback)
        } catch (t: Throwable) {
            fail("openCamera failed: $t")
        }
    }

    fun stop() {
        if (!running) return
        running = false
        runCatching { session?.stopRepeating() }
        runCatching { session?.close() }
        session = null
        runCatching { device?.close() }
        device = null
        runCatching { encoder?.signalEndOfInputStream() }
        drainThread?.join(1500)
        drainThread = null
        runCatching { encoder?.stop() }
        runCatching { encoder?.release() }
        encoder = null
        runCatching { inputSurface?.release() }
        inputSurface = null
        runCatching { sink.close() }
        thread.quitSafely()
    }

    /** Force an IDR frame — used when a new receiver joins or after packet loss. */
    fun requestKeyFrame() {
        runCatching {
            encoder?.setParameters(Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            })
        }
    }

    /**
     * Fixed exposure with the PC driving ISO. A long auto exposure (25 ms of a 33 ms
     * frame in a dim room) smears every vibration inside the frame, and no amount of
     * stabilization can undo that. [exposureNs] = 0 hands control back to auto.
     */
    fun setManualExposure(exposureNs: Long, iso: Int) {
        if (exposureNs == manualExposureNs && iso == manualIso) return
        manualExposureNs = exposureNs
        manualIso = iso
        val session = this.session ?: return
        val camera = this.device ?: return
        runCatching { session.setRepeatingRequest(buildRequest(camera), autoWatcher, handler) }
            .onFailure { Log.w(TAG, "exposure update: $it") }
    }

    @Volatile private var manualExposureNs = 0L
    @Volatile private var manualIso = 0

    /** Runtime bitrate change for network adaptation. */
    fun setBitrate(bps: Int) {
        runCatching {
            encoder?.setParameters(Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bps)
            })
            currentBitrate = bps
        }
    }

    // ---- encoder ----

    private fun startEncoder() {
        val format = MediaFormat.createVideoFormat(MIME, size.width, size.height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, currentBitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileHigh)
            // 4.1 tops out around 1080p30; 4K needs 5.2.
            setInteger(MediaFormat.KEY_LEVEL,
                if (size.width > 1920) MediaCodecInfo.CodecProfileLevel.AVCLevel52
                else MediaCodecInfo.CodecProfileLevel.AVCLevel41)
            setInteger(MediaFormat.KEY_PRIORITY, 0)          // 0 = realtime
            setInteger(MediaFormat.KEY_LATENCY, 1)           // 1 frame of encoder latency
            setFloat(MediaFormat.KEY_MAX_FPS_TO_ENCODER, fps.toFloat())
        }
        val codec = MediaCodec.createEncoderByType(MIME)
        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        inputSurface = codec.createInputSurface()
        codec.start()
        encoder = codec
        Log.i(TAG, "encoder ${codec.name} ${size.width}x${size.height}@$fps ${currentBitrate / 1000}kbps")

        drainThread = Thread(::drain, "encoder-drain").apply { start() }
    }

    private fun drain() {
        val codec = encoder ?: return
        val info = MediaCodec.BufferInfo()
        while (running) {
            val index = try {
                codec.dequeueOutputBuffer(info, 20_000)
            } catch (t: IllegalStateException) {
                break
            }
            when {
                index >= 0 -> {
                    val buf = codec.getOutputBuffer(index)
                    if (buf != null && info.size > 0) {
                        buf.position(info.offset)
                        buf.limit(info.offset + info.size)
                        runCatching { sink.onEncoded(buf, info) }
                            .onFailure { Log.w(TAG, "sink: $it") }
                    }
                    runCatching { codec.releaseOutputBuffer(index, false) }
                    if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
                }
                index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED ->
                    runCatching { sink.onFormat(codec.outputFormat) }
                        .onFailure { fail("sink format: $it") }
            }
        }
    }

    // ---- camera ----

    private val deviceCallback = object : CameraDevice.StateCallback() {
        override fun onOpened(camera: CameraDevice) {
            device = camera
            if (!running) { runCatching { camera.close() }; return }
            runCatching { configureSession(camera) }.onFailure { fail("session config: $it") }
        }

        override fun onDisconnected(camera: CameraDevice) {
            runCatching { camera.close() }
            device = null
            fail("camera disconnected")
        }

        override fun onError(camera: CameraDevice, error: Int) {
            runCatching { camera.close() }
            device = null
            fail("camera error $error")
        }
    }

    private fun configureSession(camera: CameraDevice) {
        val encoderSurface = inputSurface ?: return
        val outputs = buildList {
            add(OutputConfiguration(encoderSurface).also { applyVideoCallUseCase(it) })
            previewSurface?.let { add(OutputConfiguration(it)) }
        }
        val config = SessionConfiguration(SessionConfiguration.SESSION_REGULAR, outputs, executor,
            object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(s: CameraCaptureSession) {
                    session = s
                    if (!running) return
                    runCatching { s.setRepeatingRequest(buildRequest(camera), autoWatcher, handler) }
                        .onFailure { fail("repeating request: $it") }
                }

                override fun onConfigureFailed(s: CameraCaptureSession) = fail("session configure failed")
            })
        camera.createCaptureSession(config)
    }

    private fun applyVideoCallUseCase(oc: OutputConfiguration) {
        if (Build.VERSION.SDK_INT < 33) return
        val supported = characteristics.get(CameraCharacteristics.SCALER_AVAILABLE_STREAM_USE_CASES)
        val videoCall = CameraMetadata.SCALER_AVAILABLE_STREAM_USE_CASES_VIDEO_CALL.toLong()
        if (supported?.any { it == videoCall } == true) {
            runCatching { oc.streamUseCase = videoCall }
        }
    }

    private val characteristics: CameraCharacteristics by lazy {
        manager.getCameraCharacteristics(cameraId)
    }

    private fun buildRequest(camera: CameraDevice): CaptureRequest {
        val b = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD)
        inputSurface?.let { b.addTarget(it) }
        previewSurface?.let { b.addTarget(it) }

        // 3A: everything auto and explicitly unlocked, so the HAL keeps re-converging
        // while the light changes. Every mode is checked against what this sensor
        // actually advertises - the hidden macro/ultra-wide lack some of them.
        b.set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO)
        b.set(CaptureRequest.CONTROL_SCENE_MODE, CameraMetadata.CONTROL_SCENE_MODE_DISABLED)

        val manual = manualExposureNs > 0 && manualIso > 0
        if (manual) {
            b.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_OFF)
            b.set(CaptureRequest.SENSOR_EXPOSURE_TIME, manualExposureNs)
            b.set(CaptureRequest.SENSOR_SENSITIVITY, manualIso)
            b.set(CaptureRequest.SENSOR_FRAME_DURATION, 1_000_000_000L / fps)
        } else {
            b.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_ON)
            b.set(CaptureRequest.CONTROL_AE_LOCK, false)
            b.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, 0)
            b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, pickFpsRange())
        }
        if (!manual && supports(CameraCharacteristics.CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
                CameraMetadata.CONTROL_AE_ANTIBANDING_MODE_AUTO)) {
            // With a manual exposure the PC keeps it a multiple of the mains period, which
            // is what avoids banding; the antibanding block only applies to auto exposure.
            b.set(CaptureRequest.CONTROL_AE_ANTIBANDING_MODE,
                CameraMetadata.CONTROL_AE_ANTIBANDING_MODE_AUTO)
        }

        if (supports(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES,
                CameraMetadata.CONTROL_AWB_MODE_AUTO)) {
            b.set(CaptureRequest.CONTROL_AWB_MODE, CameraMetadata.CONTROL_AWB_MODE_AUTO)
            b.set(CaptureRequest.CONTROL_AWB_LOCK, false)
        }

        pickAfMode()?.let { b.set(CaptureRequest.CONTROL_AF_MODE, it) }

        // Meter over the whole frame instead of whatever region the HAL defaults to.
        fullFrameRegion()?.let { region ->
            val regions = arrayOf(region)
            if (maxRegions(CameraCharacteristics.CONTROL_MAX_REGIONS_AE) > 0)
                b.set(CaptureRequest.CONTROL_AE_REGIONS, regions)
            if (maxRegions(CameraCharacteristics.CONTROL_MAX_REGIONS_AWB) > 0)
                b.set(CaptureRequest.CONTROL_AWB_REGIONS, regions)
            if (maxRegions(CameraCharacteristics.CONTROL_MAX_REGIONS_AF) > 0)
                b.set(CaptureRequest.CONTROL_AF_REGIONS, regions)
        }

        // ISP blocks: some HALs leave these OFF unless asked.
        setFast(b, CameraCharacteristics.NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
            CaptureRequest.NOISE_REDUCTION_MODE, CameraMetadata.NOISE_REDUCTION_MODE_FAST)
        setFast(b, CameraCharacteristics.EDGE_AVAILABLE_EDGE_MODES,
            CaptureRequest.EDGE_MODE, CameraMetadata.EDGE_MODE_FAST)
        setFast(b, CameraCharacteristics.TONEMAP_AVAILABLE_TONE_MAP_MODES,
            CaptureRequest.TONEMAP_MODE, CameraMetadata.TONEMAP_MODE_FAST)
        setFast(b, CameraCharacteristics.HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES,
            CaptureRequest.HOT_PIXEL_MODE, CameraMetadata.HOT_PIXEL_MODE_FAST)
        setFast(b, CameraCharacteristics.SHADING_AVAILABLE_MODES,
            CaptureRequest.SHADING_MODE, CameraMetadata.SHADING_MODE_FAST)
        setFast(b, CameraCharacteristics.COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
            CaptureRequest.COLOR_CORRECTION_ABERRATION_MODE,
            CameraMetadata.COLOR_CORRECTION_ABERRATION_MODE_FAST)

        Log.i(TAG, "fps range picked=${pickFpsRange()} available=" +
                characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
                    ?.joinToString())

        val stabilization = pickStabilization()
        b.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, stabilization)
        Log.i(TAG, "stabilization=$stabilization available=" +
                characteristics.get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES)
                    ?.joinToString())
        if (hasOis()) {
            // OIS shifts the image by an amount the gyro does not see, so it has to go
            // when the PC does the stabilizing.
            b.set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE,
                if (stabilizeOnDevice) CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_ON
                else CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_OFF)
        }
        return b.build()
    }

    /** Reports 3A convergence so the UI can show whether the picture has settled. */
    private val autoWatcher = object : CameraCaptureSession.CaptureCallback() {
        override fun onCaptureCompleted(
            session: CameraCaptureSession,
            request: CaptureRequest,
            result: TotalCaptureResult
        ) {
            val af = when (result.get(CaptureResult.CONTROL_AF_STATE)) {
                null -> "фикс"
                CameraMetadata.CONTROL_AF_STATE_PASSIVE_SCAN,
                CameraMetadata.CONTROL_AF_STATE_ACTIVE_SCAN -> "наводится"
                CameraMetadata.CONTROL_AF_STATE_PASSIVE_FOCUSED,
                CameraMetadata.CONTROL_AF_STATE_FOCUSED_LOCKED -> "наведён"
                CameraMetadata.CONTROL_AF_STATE_NOT_FOCUSED_LOCKED,
                CameraMetadata.CONTROL_AF_STATE_PASSIVE_UNFOCUSED -> "не поймал"
                else -> "ждёт"
            }
            val ae = when (result.get(CaptureResult.CONTROL_AE_STATE)) {
                CameraMetadata.CONTROL_AE_STATE_CONVERGED,
                CameraMetadata.CONTROL_AE_STATE_LOCKED -> "готово"
                CameraMetadata.CONTROL_AE_STATE_SEARCHING,
                CameraMetadata.CONTROL_AE_STATE_PRECAPTURE -> "подбор"
                CameraMetadata.CONTROL_AE_STATE_FLASH_REQUIRED -> "мало света"
                else -> "-"
            }
            val awb = when (result.get(CaptureResult.CONTROL_AWB_STATE)) {
                CameraMetadata.CONTROL_AWB_STATE_CONVERGED,
                CameraMetadata.CONTROL_AWB_STATE_LOCKED -> "готово"
                CameraMetadata.CONTROL_AWB_STATE_SEARCHING -> "подбор"
                else -> "-"
            }
            rtpSink?.exposureNs = result.get(CaptureResult.SENSOR_EXPOSURE_TIME) ?: 0L
            rtpSink?.rollingShutterSkewNs = result.get(CaptureResult.SENSOR_ROLLING_SHUTTER_SKEW) ?: 0L

            val sensorTs = result.get(CaptureResult.SENSOR_TIMESTAMP) ?: 0L
            if (framesLogged < 3) {
                Log.i(TAG, "SENSOR_TIMESTAMP=$sensorTs elapsedRealtimeNanos=" +
                        android.os.SystemClock.elapsedRealtimeNanos() +
                        " uptimeNanos=" + System.nanoTime())
            }

            val iso = result.get(CaptureResult.SENSOR_SENSITIVITY)
            val stab = stabilizationName(result.get(CaptureResult.CONTROL_VIDEO_STABILIZATION_MODE))
            val exposureUs = (result.get(CaptureResult.SENSOR_EXPOSURE_TIME) ?: 0L) / 1000
            val frameUs = (result.get(CaptureResult.SENSOR_FRAME_DURATION) ?: 0L) / 1000
            if (framesLogged++ % 60 == 0L) {
                Log.i(TAG, "exposure=${exposureUs}us frameDuration=${frameUs}us " +
                        "(=${if (frameUs > 0) 1_000_000 / frameUs else 0} fps)")
            }
            val text = "фокус: $af · экспозиция: $ae · ББ: $awb · $stab" +
                    if (iso != null) " · ISO $iso" else ""
            if (text != lastAuto) {
                lastAuto = text
                onAuto(text)
            }
        }
    }

    @Volatile private var lastAuto = ""
    private var framesLogged = 0L

    private fun supports(key: CameraCharacteristics.Key<IntArray>, value: Int) =
        characteristics.get(key)?.contains(value) == true

    private fun maxRegions(key: CameraCharacteristics.Key<Int>) = characteristics.get(key) ?: 0

    private fun setFast(
        b: CaptureRequest.Builder,
        available: CameraCharacteristics.Key<IntArray>,
        key: CaptureRequest.Key<Int>,
        value: Int
    ) {
        if (supports(available, value)) b.set(key, value)
    }

    /** Continuous video AF where available; fixed-focus sensors report only OFF. */
    private fun pickAfMode(): Int? {
        val modes = characteristics.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES) ?: return null
        return listOf(
            CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO,
            CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE,
            CameraMetadata.CONTROL_AF_MODE_AUTO
        ).firstOrNull { modes.contains(it) }
    }

    private fun fullFrameRegion(): MeteringRectangle? {
        val active = characteristics.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) ?: return null
        return MeteringRectangle(0, 0, active.width(), active.height(), MeteringRectangle.METERING_WEIGHT_MAX)
    }

    private fun pickFpsRange(): Range<Int> {
        val ranges = characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?: return Range(fps, fps)
        return ranges.firstOrNull { it.lower == fps && it.upper == fps }
            ?: ranges.filter { it.upper == fps }.maxByOrNull { it.lower }
            ?: ranges.filter { it.contains(fps) }.minByOrNull { it.upper - it.lower }
            ?: Range(fps, fps)
    }

    /**
     * Gyro-based EIS. PREVIEW_STABILIZATION (API 33+) is the newer, much steadier
     * mode intended for streaming; plain ON is the legacy one.
     */
    private fun pickStabilization(): Int {
        if (!stabilizeOnDevice) return CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_OFF
        val modes = characteristics.get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES)
            ?: return CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_OFF
        val preview = CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_PREVIEW_STABILIZATION
        return when {
            Build.VERSION.SDK_INT >= 33 && modes.contains(preview) -> preview
            modes.contains(CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_ON) ->
                CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_ON
            else -> CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_OFF
        }
    }

    private fun stabilizationName(mode: Int?) = when (mode) {
        CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_PREVIEW_STABILIZATION -> "стаб: preview"
        CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_ON -> "стаб: вкл"
        else -> "стаб: выкл"
    }

    private fun hasOis(): Boolean =
        characteristics.get(CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION)
            ?.any { it == CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_ON } == true

    private fun fail(message: String) {
        Log.e(TAG, message)
        onError(message)
    }

    companion object {
        const val MIME = MediaFormat.MIMETYPE_VIDEO_AVC
        private const val TAG = "CameraStreamer"
    }
}
