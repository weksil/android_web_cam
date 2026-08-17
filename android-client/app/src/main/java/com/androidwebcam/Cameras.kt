package com.androidwebcam

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.media.MediaCodec
import android.util.Size

object Cameras {

    class Geometry(val fx: Float, val fy: Float, val cx: Float, val cy: Float, val orientation: Int)

    class SensorLimits(val isoMin: Int, val isoMax: Int,
                       val exposureMinNs: Long, val exposureMaxNs: Long)

    fun sensorLimits(context: Context, id: String): SensorLimits? {
        val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val c = runCatching { manager.getCameraCharacteristics(id) }.getOrNull() ?: return null
        val iso = c.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE) ?: return null
        val exposure = c.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE) ?: return null
        return SensorLimits(iso.lower, iso.upper, exposure.lower, exposure.upper)
    }

    /**
     * Camera intrinsics rescaled from the active sensor array to the streamed frame.
     * A 16:9 stream is a full-width, vertically centred crop of the 4:3 array, so the
     * principal point shifts by the crop offset and both axes share one scale factor.
     */
    fun geometryFor(context: Context, id: String, width: Int, height: Int): Geometry? {
        val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val c = runCatching { manager.getCameraCharacteristics(id) }.getOrNull() ?: return null
        val active = c.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) ?: return null
        val orientation = c.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0

        val calibration = c.get(CameraCharacteristics.LENS_INTRINSIC_CALIBRATION)
        val scale = width.toFloat() / active.width()
        val cropHeight = active.width().toFloat() * height / width
        val cropY = (active.height() - cropHeight) / 2f

        if (calibration != null && calibration.size >= 4 && calibration[0] > 0f) {
            return Geometry(
                fx = calibration[0] * scale,
                fy = calibration[1] * scale,
                cx = calibration[2] * scale,
                cy = (calibration[3] - cropY) * scale,
                orientation = orientation
            )
        }
        // Fallback: derive the focal length in pixels from the physical sensor size.
        val focal = c.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.firstOrNull()
            ?: return null
        val physical = c.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE) ?: return null
        val fx = focal / physical.width * width
        return Geometry(fx, fx, width / 2f, height / 2f, orientation)
    }

    /** What else this hardware offers that we are not using yet. */
    fun probeHardware(context: Context, id: String): String {
        val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val c = manager.getCameraCharacteristics(id)
        val map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)

        val highSpeed = map?.highSpeedVideoSizes?.joinToString { size ->
            "$size@" + map.getHighSpeedVideoFpsRangesFor(size).joinToString("/") { "${it.upper}" }
        }
        val faceModes = c.get(CameraCharacteristics.STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES)
            ?.joinToString()
        val oisModes = if (android.os.Build.VERSION.SDK_INT >= 28)
            c.get(CameraCharacteristics.STATISTICS_INFO_AVAILABLE_OIS_DATA_MODES)?.joinToString()
        else null
        val encoders = android.media.MediaCodecList(android.media.MediaCodecList.REGULAR_CODECS)
            .codecInfos.filter { it.isEncoder && it.supportedTypes.any { t -> t.startsWith("video/") } }
            .joinToString { "${it.name}:${it.supportedTypes.joinToString("|")}" }

        return buildString {
            appendLine("camera $id extras")
            appendLine("  highSpeed: $highSpeed")
            appendLine("  faceDetect modes: $faceModes  maxFaces=${c.get(CameraCharacteristics.STATISTICS_INFO_MAX_FACE_COUNT)}")
            appendLine("  OIS data modes: $oisModes")
            appendLine("  apertures: ${c.get(CameraCharacteristics.LENS_INFO_AVAILABLE_APERTURES)?.joinToString()}")
            appendLine("  zoomRange: ${if (android.os.Build.VERSION.SDK_INT >= 30) c.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) else null}")
            appendLine("  tonemapModes: ${c.get(CameraCharacteristics.TONEMAP_AVAILABLE_TONE_MAP_MODES)?.joinToString()}")
            appendLine("  edge/nr modes: ${c.get(CameraCharacteristics.EDGE_AVAILABLE_EDGE_MODES)?.joinToString()} / ${c.get(CameraCharacteristics.NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES)?.joinToString()}")
            appendLine("  capabilities: ${c.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)?.joinToString()}")
            appendLine("  encoders: $encoders")
        }
    }

    /** Everything gyro-based stabilization on the PC needs to know about a sensor. */
    fun probeStabilizationInputs(context: Context, id: String): String {
        val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val c = manager.getCameraCharacteristics(id)
        val map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val big = map?.getOutputSizes(MediaCodec::class.java)
            ?.filter { it.width >= 1920 }?.sortedByDescending { it.width }?.joinToString()
        val physical = c.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE)
        val active = c.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)
        val pixels = c.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE)
        val timestampSource = c.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE)
        val intrinsics = c.get(CameraCharacteristics.LENS_INTRINSIC_CALIBRATION)?.joinToString()
        val distortion = c.get(CameraCharacteristics.LENS_DISTORTION)?.joinToString()
        val fps = map?.getHighSpeedVideoFpsRanges()?.joinToString()

        val sensors = context.getSystemService(Context.SENSOR_SERVICE) as android.hardware.SensorManager
        val gyro = sensors.getDefaultSensor(android.hardware.Sensor.TYPE_GYROSCOPE)
        val uncal = sensors.getDefaultSensor(android.hardware.Sensor.TYPE_GYROSCOPE_UNCALIBRATED)

        return buildString {
            appendLine("camera $id")
            appendLine("  big sizes: $big")
            appendLine("  physical=$physical active=$active pixelArray=$pixels")
            appendLine("  orientation=${c.get(CameraCharacteristics.SENSOR_ORIENTATION)} " +
                    "focal=${c.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.joinToString()}")
            appendLine("  timestampSource=$timestampSource (1 = REALTIME, shares the clock with sensors)")
            appendLine("  intrinsics=$intrinsics distortion=$distortion")
            appendLine("  stabModes=${c.get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES)?.joinToString()}")
            appendLine("  highSpeedFps=$fps")
            appendLine("  gyro=${gyro?.name} minDelay=${gyro?.minDelay}us res=${gyro?.resolution} range=${gyro?.maximumRange}")
            appendLine("  gyroUncal=${uncal?.name} minDelay=${uncal?.minDelay}us")
        }
    }

    data class Info(
        val id: String,
        val facing: Int,
        val sizes: List<Size>,
        val focal: Float,
        val listed: Boolean,
        val label: String
    )

    /**
     * Sensors usable for video capture.
     *
     * [CameraManager.getCameraIdList] only reports the two "primary" cameras on most
     * phones; the ultra-wide, macro and tele sensors sit behind ids the system does
     * not enumerate but which can still be read and opened. We probe those too, then
     * collapse ids that are the same physical sensor (identical facing and focal
     * length - vendors expose the main camera several times).
     */
    fun list(context: Context): List<Info> {
        val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val listed = manager.cameraIdList.toSet()
        val ids = (listed + (0..9).map { it.toString() }).distinct()

        val found = ids.mapNotNull { id -> read(manager, id, id in listed) }
        val mainFocal = found.filter { it.listed && it.facing == CameraCharacteristics.LENS_FACING_BACK }
            .minOfOrNull { it.focal } ?: 0f

        return found
            .groupBy { it.facing to Math.round(it.focal * 100) }
            .map { (_, same) -> same.sortedWith(compareBy({ !it.listed }, { it.id.toIntOrNull() ?: 99 })).first() }
            // back first, main sensor first, then by resolution: the 2 MP macro last
            .sortedWith(compareBy({ if (it.facing == CameraCharacteristics.LENS_FACING_BACK) 0 else 1 },
                { !it.listed },
                { -it.sizes.first().let { s -> s.width.toLong() * s.height } }))
            .map { it.copy(label = label(it, mainFocal)) }
    }

    private fun read(manager: CameraManager, id: String, listed: Boolean): Info? {
        val c = runCatching { manager.getCameraCharacteristics(id) }.getOrNull() ?: return null
        val map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return null
        val sizes = map.getOutputSizes(MediaCodec::class.java)
            ?.filter { it.width >= 640 && isWide(it) }
            ?.sortedByDescending { it.width.toLong() * it.height }
            ?: return null
        if (sizes.isEmpty()) return null
        return Info(
            id = id,
            facing = c.get(CameraCharacteristics.LENS_FACING) ?: -1,
            sizes = sizes,
            focal = c.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.firstOrNull() ?: 0f,
            listed = listed,
            label = ""
        )
    }

    private fun label(info: Info, mainFocal: Float): String {
        val side = if (info.facing == CameraCharacteristics.LENS_FACING_FRONT) "Фронт" else "Тыл"
        val megapixels = info.sizes.first().let { it.width.toLong() * it.height / 1_000_000.0 }
        val lens = when {
            info.facing == CameraCharacteristics.LENS_FACING_FRONT -> ""
            mainFocal <= 0f || info.focal <= 0f -> ""
            info.focal >= mainFocal -> if (info.focal > mainFocal * 1.5f) " теле" else " основной"
            megapixels < 3.0 -> " макро"
            else -> " ширик"
        }
        return "$side$lens ${"%.1f".format(info.focal)}мм (${info.id})"
    }

    /** 16:9 (+/- rounding). Webcam consumers expect widescreen. */
    private fun isWide(s: Size): Boolean {
        val ratio = s.width.toFloat() / s.height
        return ratio > 1.7f && ratio < 1.81f
    }
}
