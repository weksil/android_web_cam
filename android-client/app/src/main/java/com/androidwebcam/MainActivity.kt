package com.androidwebcam

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Size
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.Spinner
import android.widget.TextView
import java.io.File
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.random.Random

class MainActivity : Activity(), SurfaceHolder.Callback {

    private enum class Mode { STREAM, FILE }

    private lateinit var aspectView: AspectFrameLayout
    private lateinit var surfaceView: SurfaceView
    private lateinit var statusView: TextView
    private lateinit var startButton: Button
    private lateinit var modeSpinner: Spinner
    private lateinit var cameraSpinner: Spinner
    private lateinit var sizeSpinner: Spinner
    private lateinit var bitrateSpinner: Spinner

    private val ui = Handler(Looper.getMainLooper())

    private var cameras: List<Cameras.Info> = emptyList()
    private var streamer: CameraStreamer? = null
    private var rtpSink: RtpSink? = null
    private var fileSink: Mp4FileSink? = null
    private var outFile: File? = null
    private var server: ControlServer? = null
    private var telemetry: Telemetry? = null
    @Volatile private var eisOnDevice = true

    @Volatile private var activeCameraId: String? = null
    @Volatile private var activeSize: Size? = null
    @Volatile private var activeBitrate = 0
    @Volatile private var peerLabel: String? = null
    @Volatile private var autoState = ""

    private var surfaceSize: Size? = null
    private var pendingAction: (() -> Unit)? = null
    private var lastBytes = 0L
    private var running = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Real screen-on lock: the manifest attribute of the same name only applies
        // to views and is silently ignored on <activity>.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContentView(buildUi())
        surfaceView.holder.addCallback(this)
        if (hasCameraPermission()) loadCameras()
        else requestPermissions(arrayOf(Manifest.permission.CAMERA), REQ_CAMERA)
    }

    /** Switch the sensor of a running stream from adb: am start ... --es cameraId 3 */
    override fun onNewIntent(intent: android.content.Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        val wanted = intent.getStringExtra("cameraId") ?: return
        val index = cameras.indexOfFirst { it.id == wanted }
        if (index >= 0) {
            cameraSpinner.setSelection(index)
            onCameraSelected(index)
        }
    }

    // Stop on onStop, not onPause: a transient overlay (dialog, new intent delivery)
    // pauses the activity without hiding it, and must not kill the stream.
    override fun onStop() {
        super.onStop()
        if (running) stopAll()
    }

    override fun onRequestPermissionsResult(code: Int, perms: Array<out String>, results: IntArray) {
        super.onRequestPermissionsResult(code, perms, results)
        if (code == REQ_CAMERA && results.firstOrNull() == PackageManager.PERMISSION_GRANTED) loadCameras()
        else status("Нет разрешения на камеру")
    }

    // ---- ui ----

    private fun buildUi(): View {
        val root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }

        surfaceView = SurfaceView(this)
        aspectView = AspectFrameLayout(this).apply {
            addView(surfaceView, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        }
        root.addView(aspectView, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT, Gravity.CENTER))

        modeSpinner = Spinner(this).apply { adapter = adapterOf(listOf("Сеть (RTP)", "Файл (mp4)")) }
        cameraSpinner = Spinner(this)
        sizeSpinner = Spinner(this)
        bitrateSpinner = Spinner(this).apply {
            adapter = adapterOf(BITRATES.map { "${it / 1_000_000} Mbps" })
            setSelection(BITRATES.indexOf(DEFAULT_BITRATE).coerceAtLeast(0))
        }
        startButton = Button(this).apply {
            text = "Старт"
            setOnClickListener { if (running) stopAll() else start() }
        }
        statusView = TextView(this).apply {
            setTextColor(Color.WHITE)
            textSize = 12f
            text = "Инициализация…"
        }
        cameraSpinner.onItemSelected { onCameraSelected(it) }

        val panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.argb(170, 0, 0, 0))
            setPadding(24, 16, 24, 16)
            addView(modeSpinner)
            addView(cameraSpinner)
            addView(sizeSpinner)
            addView(bitrateSpinner)
            addView(startButton)
            addView(statusView)
        }
        root.addView(panel, FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.START or Gravity.TOP))
        return root
    }

    private fun adapterOf(items: List<String>) =
        ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, items)

    private inline fun Spinner.onItemSelected(crossinline body: (Int) -> Unit) {
        onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: android.widget.AdapterView<*>?, v: View?, pos: Int, id: Long) = body(pos)
            override fun onNothingSelected(p: android.widget.AdapterView<*>?) = Unit
        }
    }

    private fun loadCameras() {
        cameras = runCatching { Cameras.list(this) }.getOrElse {
            status("Ошибка перечисления камер: $it"); emptyList()
        }
        if (cameras.isEmpty()) { status("Подходящих камер не найдено"); return }
        android.util.Log.i("Cameras", cameras.joinToString(" | ") { "${it.label} max=${it.sizes.first()}" })
        cameras.firstOrNull()?.let {
            android.util.Log.i("Gyro", Cameras.probeStabilizationInputs(this, it.id))
            android.util.Log.i("Hardware", Cameras.probeHardware(this, it.id))
        }
        cameraSpinner.adapter = adapterOf(cameras.map { it.label })
        onCameraSelected(0)
        status("Готово. IP: ${localIp() ?: "нет сети"}")

        // adb-friendly headless start:
        //   am start -n com.androidwebcam/.MainActivity --ez autostart true \
        //     [--ei mode 1] [--ei width 1280] [--ei bitrate 20000000]
        if (intent?.getBooleanExtra("autostart", false) == true) {
            modeSpinner.setSelection(intent.getIntExtra("mode", 0))
            intent.getStringExtra("cameraId")?.let { wanted ->
                val index = cameras.indexOfFirst { it.id == wanted }
                if (index >= 0) {
                    cameraSpinner.setSelection(index)
                    onCameraSelected(index)     // rebuild sizes now, not on the next layout
                }
            }
            val width = intent.getIntExtra("width", 0)
            if (width > 0) {
                val index = selectedCamera()?.sizes?.indexOfFirst { it.width == width } ?: -1
                if (index >= 0) sizeSpinner.setSelection(index)
            }
            bitrateOverride = intent.getIntExtra("bitrate", 0)
            ui.post { if (!running) start() }
        }
    }

    private fun onCameraSelected(index: Int) {
        val cam = cameras.getOrNull(index) ?: return
        val previous = (sizeSpinner.selectedItem as? String)
        sizeSpinner.adapter = adapterOf(cam.sizes.map { "${it.width}x${it.height}" })
        val keep = cam.sizes.indexOfFirst { "${it.width}x${it.height}" == previous }
        // 2560x1440 by default: enough margin for stabilization and downscaling to 1080p,
        // without the encoder load of 4K that pushes the phone into thermal throttling.
        val preferred = DEFAULT_SIZES.firstNotNullOfOrNull { wanted ->
            cam.sizes.indexOfFirst { it.width == wanted }.takeIf { it >= 0 }
        } ?: 0
        sizeSpinner.setSelection(if (keep >= 0) keep else preferred)

        if (running && mode() == Mode.STREAM) switchSensor(cam)
    }

    /** Switching the sensor mid-stream: restart capture, keep the link to the PC. */
    private fun switchSensor(cam: Cameras.Info) {
        activeCameraId = cam.id
        val size = selectedSize() ?: return
        activeSize = size
        if (streamer == null) return          // linked but idle, nothing to restart yet
        stopStreamer()
        withSurfaceReady(size) { onStreamStart() }
    }

    private fun selectedCamera() = cameras.getOrNull(cameraSpinner.selectedItemPosition)
    private fun selectedSize() = selectedCamera()?.sizes?.getOrNull(sizeSpinner.selectedItemPosition)
    private var bitrateOverride = 0

    private fun selectedBitrate() =
        if (bitrateOverride > 0) bitrateOverride
        else BITRATES[bitrateSpinner.selectedItemPosition.coerceIn(BITRATES.indices)]
    private fun mode() = if (modeSpinner.selectedItemPosition == 0) Mode.STREAM else Mode.FILE

    private fun status(text: String) = runOnUiThread { statusView.text = text }

    // ---- lifecycle ----

    private fun start() {
        if (!hasCameraPermission()) {
            requestPermissions(arrayOf(Manifest.permission.CAMERA), REQ_CAMERA); return
        }
        val cam = selectedCamera() ?: return
        val size = selectedSize() ?: return
        activeCameraId = cam.id
        activeSize = size
        activeBitrate = selectedBitrate()
        running = true
        startButton.text = "Стоп"
        setControlsEnabled(false)

        withSurfaceReady(size) {
            when (mode()) {
                Mode.STREAM -> startServer()
                Mode.FILE -> startFileCapture()
            }
        }
    }

    private fun stopAll() {
        running = false
        pendingAction = null
        server?.stop()
        server = null
        stopStreamer()
        ui.removeCallbacks(ticker)
        startButton.text = "Старт"
        setControlsEnabled(true)
        val file = outFile
        status(
            if (file != null && file.exists())
                "Файл: ${fileFrames} кадров, ${file.length() / 1024} КБ\n${file.absolutePath}"
            else "Остановлено. IP: ${localIp() ?: "нет сети"}"
        )
    }

    private var fileFrames = 0

    private fun withSurfaceReady(size: Size, action: () -> Unit) {
        aspectView.aspect = size.width.toFloat() / size.height
        surfaceView.holder.setFixedSize(size.width, size.height)
        if (surfaceSize == size) action() else {
            pendingAction = action
            status("Ожидание surface ${size.width}x${size.height}…")
        }
    }

    // ---- streaming ----

    private fun startServer() {
        val ip = localIp()
        server = ControlServer(
            deviceName = Build.MODEL,
            onLink = { address, port -> onLinked(address, port) },
            onUnlink = { runOnUiThread { onUnlinked(ip) } },
            onStreamStart = { runOnUiThread { onStreamStart() } },
            onStreamStop = { runOnUiThread { onStreamStop() } },
            onIdr = { streamer?.requestKeyFrame() },
            onBitrate = { bps -> streamer?.setBitrate(bps) },
            onEis = { onDevice -> runOnUiThread { setDeviceEis(onDevice) } },
            onExposure = { exposureNs, iso -> streamer?.setManualExposure(exposureNs, iso) },
            onPeer = { label -> peerLabel = label }
        ).also {
            runCatching { it.start() }.onFailure { e -> status("Порт ${Control.CONTROL_PORT} занят: $e") }
        }
        status(waitingText(ip))
    }

    private fun waitingText(ip: String?) =
        "Ожидание ПК…\nIP: ${ip ?: "нет сети"}  порт ${Control.CONTROL_PORT}"

    /** HELLO: allocate the RTP socket and answer with our capabilities. Camera stays off. */
    private fun onLinked(address: InetAddress, port: Int): StreamInfo? {
        val size = activeSize ?: return null
        activeCameraId ?: return null
        rtpSink?.release()
        val ssrc = Random.nextInt()
        val sink = RtpSink(address, port, ssrc)
        rtpSink = sink
        runOnUiThread { status("ПК подключён: ${address.hostAddress}\nкамера выключена, ждём запроса видео") }
        return StreamInfo(size.width, size.height, FPS, activeBitrate, ssrc, sink.localPort)
    }

    private fun onUnlinked(ip: String?) {
        stopStreamer()
        rtpSink?.release()
        rtpSink = null
        ui.removeCallbacks(ticker)
        if (running) status(waitingText(ip))
    }

    private fun onStreamStart() {
        val size = activeSize ?: return
        val camId = activeCameraId ?: return
        val sink = rtpSink ?: return
        if (streamer != null) return
        if (!launchStreamer(camId, size, sink)) return
        startTelemetry(camId, size, sink)
        lastBytes = 0
        ui.removeCallbacks(ticker)
        ui.postDelayed(ticker, 1000)
    }

    private fun onStreamStop() {
        stopStreamer()
        ui.removeCallbacks(ticker)
        status("ПК подключён: ${peerLabel ?: "?"}\nкамера выключена, ждём запроса видео")
    }

    /** The PC asked to take stabilization over; on-device EIS has to step aside. */
    private fun setDeviceEis(onDevice: Boolean) {
        if (eisOnDevice == onDevice) return
        eisOnDevice = onDevice
        if (streamer != null) {          // re-issue the capture request with the new mode
            val size = activeSize ?: return
            stopStreamer()
            withSurfaceReady(size) { onStreamStart() }
        }
    }

    private fun startTelemetry(cameraId: String, size: Size, sink: RtpSink) {
        telemetry?.stop()
        val t = Telemetry(this) { payload -> sink.sendAux(payload) }
        telemetry = t
        Cameras.geometryFor(this, cameraId, size.width, size.height)?.let { g ->
            t.sendCameraInfo(g.fx, g.fy, g.cx, g.cy, size.width, size.height, g.orientation,
                selectedCamera()?.facing ?: -1)
        }
        Cameras.sensorLimits(this, cameraId)?.let { limits ->
            t.sendSensorLimits(limits.isoMin, limits.isoMax, limits.exposureMinNs, limits.exposureMaxNs)
        }
        t.start()
    }

    private val ticker = object : Runnable {
        override fun run() {
            val sink = rtpSink
            if (running && sink != null) {
                val kbps = (sink.bytesSent - lastBytes) * 8 / 1000
                lastBytes = sink.bytesSent
                status(
                    "Стрим -> ${peerLabel ?: "?"}\n" +
                            "${activeSize?.width}x${activeSize?.height}@$FPS  $kbps kbps\n" +
                            "кадров ${sink.framesSent}, пакетов ${sink.packetsSent}\n" +
                            autoState
                )
                ui.postDelayed(this, 1000)
            }
        }
    }

    private fun startFileCapture() {
        val size = activeSize ?: return
        val camId = activeCameraId ?: return
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val file = File(getExternalFilesDir(null) ?: filesDir, "capture-$stamp.mp4")
        val sink = Mp4FileSink(file)
        fileSink = sink
        outFile = file
        if (launchStreamer(camId, size, sink)) {
            status("Пишем ${size.width}x${size.height}@$FPS\n${file.name}")
        }
    }

    private fun launchStreamer(cameraId: String, size: Size, sink: VideoSink): Boolean {
        stopStreamer()
        val surface = surfaceView.holder.surface
        if (surface?.isValid != true) { status("Surface не готов"); return false }
        val s = CameraStreamer(
            context = this,
            cameraId = cameraId,
            size = size,
            fps = FPS,
            bitrate = activeBitrate,
            sink = sink,
            previewSurface = surface,
            onError = { msg -> status("Ошибка: $msg") },
            onAuto = { text -> autoState = text },
            stabilizeOnDevice = eisOnDevice
        )
        streamer = s
        s.start()
        return true
    }

    /** Stops capture only. [rtpSink] belongs to the link and outlives it. */
    private fun stopStreamer() {
        fileFrames = fileSink?.frameCount ?: fileFrames
        telemetry?.stop()
        telemetry = null
        streamer?.stop()
        streamer = null
        fileSink = null
    }

    private fun setControlsEnabled(enabled: Boolean) {
        modeSpinner.isEnabled = enabled
        sizeSpinner.isEnabled = enabled
        bitrateSpinner.isEnabled = enabled
        cameraSpinner.isEnabled = enabled || mode() == Mode.STREAM   // sensor is switchable live
    }

    private fun hasCameraPermission() =
        checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

    private fun localIp(): String? =
        NetworkInterface.getNetworkInterfaces().toList()
            .filter { it.isUp && !it.isLoopback }
            .flatMap { it.inetAddresses.toList() }
            .filterIsInstance<Inet4Address>()
            .firstOrNull()?.hostAddress

    // ---- surface ----

    override fun surfaceCreated(holder: SurfaceHolder) = Unit

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        surfaceSize = Size(width, height)
        val action = pendingAction
        if (action != null && surfaceSize == activeSize) {
            pendingAction = null
            action()
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        surfaceSize = null
        if (running) stopAll()
    }

    private companion object {
        const val REQ_CAMERA = 1
        const val FPS = 30
        val BITRATES = listOf(
            4_000_000, 6_000_000, 8_000_000, 12_000_000,
            16_000_000, 20_000_000, 25_000_000, 30_000_000, 40_000_000
        )
        const val DEFAULT_BITRATE = 20_000_000
        val DEFAULT_SIZES = listOf(2560, 1920)
    }
}
