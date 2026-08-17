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
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.random.Random

/**
 * The phone is the capture device, not the control panel: camera, resolution, frame rate,
 * bitrate and codec all come from the PC over the control channel. This screen only shows
 * status, and it blanks itself while streaming to save power.
 */
class MainActivity : Activity(), SurfaceHolder.Callback {

    private lateinit var aspectView: AspectFrameLayout
    private lateinit var surfaceView: SurfaceView
    private lateinit var statusView: TextView
    private lateinit var startButton: Button
    private lateinit var panel: LinearLayout

    private val ui = Handler(Looper.getMainLooper())

    private var cameras: List<Cameras.Info> = emptyList()
    private var streamer: CameraStreamer? = null
    private var rtpSink: RtpSink? = null
    private var fileSink: Mp4FileSink? = null
    private var outFile: File? = null
    private var server: ControlServer? = null
    private var telemetry: Telemetry? = null

    // Configuration owned by the PC; these are only the fallbacks used until it speaks up.
    @Volatile private var activeCameraId: String? = null
    @Volatile private var activeSize: Size? = null
    @Volatile private var activeFps = 30
    @Volatile private var activeBitrate = 20_000_000
    @Volatile private var useHevc = false
    @Volatile private var eisOnDevice = true
    @Volatile private var peerLabel: String? = null
    @Volatile private var autoState = ""

    private var fileMode = false
    private var surfaceSize: Size? = null
    private var pendingAction: (() -> Unit)? = null
    private var lastBytes = 0L
    private var running = false
    private var blanked = false
    private var fileFrames = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContentView(buildUi())
        surfaceView.holder.addCallback(this)
        if (hasCameraPermission()) loadCameras()
        else requestPermissions(arrayOf(Manifest.permission.CAMERA), REQ_CAMERA)
    }

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

        startButton = Button(this).apply {
            text = "Старт"
            setOnClickListener { if (running) stopAll() else start() }
        }
        statusView = TextView(this).apply {
            setTextColor(Color.WHITE)
            textSize = 12f
            text = "Инициализация…"
        }
        panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.argb(170, 0, 0, 0))
            setPadding(24, 16, 24, 16)
            addView(startButton)
            addView(statusView)
        }
        root.addView(panel, FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.START or Gravity.TOP))

        // Any touch brings the screen back for a while; it blanks itself again after.
        root.setOnClickListener { wakeScreen() }
        return root
    }

    private fun loadCameras() {
        cameras = runCatching { Cameras.list(this) }.getOrElse {
            status("Ошибка перечисления камер: $it"); emptyList()
        }
        if (cameras.isEmpty()) { status("Подходящих камер не найдено"); return }

        val first = cameras.first()
        activeCameraId = first.id
        activeSize = first.sizes.firstOrNull { it.width == 2560 }
            ?: first.sizes.firstOrNull { it.width == 1920 } ?: first.sizes.first()
        android.util.Log.i("Cameras", cameras.joinToString(" | ") { "${it.label} max=${it.sizes.first()}" })
        status("Готово. IP: ${localIp() ?: "нет сети"}")

        if (intent?.getBooleanExtra("autostart", false) == true) {
            fileMode = intent.getIntExtra("mode", 0) == 1
            // Diagnostic overrides: the PC normally owns these, but --test does not send a
            // configuration, so without them a mode cannot be reproduced from a shell.
            val width = intent.getIntExtra("width", 0)
            if (width > 0) {
                first.modes.firstOrNull { it.size.width == width }?.let { activeSize = it.size }
            }
            intent.getIntExtra("fps", 0).takeIf { it > 0 }?.let { activeFps = it }
            intent.getIntExtra("bitrate", 0).takeIf { it > 0 }?.let { activeBitrate = it }
            useHevc = intent.getIntExtra("codec", 0) == 1
            ui.post { if (!running) start() }
        }
    }

    private fun status(text: String) = runOnUiThread { statusView.text = text }

    // ---- screen power ----

    /** Black screen at zero brightness and no preview stream: the panel and the extra
     *  camera output are the two things worth switching off while nobody is looking. */
    private fun blankScreen() {
        if (blanked) return
        blanked = true
        ui.removeCallbacks(blankLater)
        window.attributes = window.attributes.apply { screenBrightness = 0f }
        panel.visibility = View.GONE
        aspectView.visibility = View.GONE
        streamer?.setPreviewEnabled(false)
    }

    private fun wakeScreen() {
        blanked = false
        window.attributes = window.attributes.apply {
            screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        }
        panel.visibility = View.VISIBLE
        aspectView.visibility = View.VISIBLE
        streamer?.setPreviewEnabled(true)
        ui.removeCallbacks(blankLater)
        if (running) ui.postDelayed(blankLater, SCREEN_ON_MS)
    }

    private val blankLater = Runnable { if (running && streamer != null) blankScreen() }

    // ---- lifecycle ----

    private fun start() {
        if (!hasCameraPermission()) {
            requestPermissions(arrayOf(Manifest.permission.CAMERA), REQ_CAMERA); return
        }
        val size = activeSize ?: return
        running = true
        startButton.text = "Стоп"
        withSurfaceReady(size) { if (fileMode) startFileCapture() else startServer() }
    }

    private fun stopAll() {
        running = false
        pendingAction = null
        server?.stop()
        server = null
        stopStreamer()
        rtpSink?.release()
        rtpSink = null
        ui.removeCallbacks(ticker)
        ui.removeCallbacks(blankLater)
        wakeScreen()
        startButton.text = "Старт"
        val file = outFile
        status(
            if (file != null && file.exists())
                "Файл: $fileFrames кадров, ${file.length() / 1024} КБ\n${file.absolutePath}"
            else "Остановлено. IP: ${localIp() ?: "нет сети"}"
        )
    }

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
            onConfig = { id, w, h, fps, bitrate, hevc ->
                runOnUiThread { applyConfig(id, w, h, fps, bitrate, hevc) }
            },
            onPeer = { label -> peerLabel = label }
        ).also {
            runCatching { it.start() }.onFailure { e -> status("Порт ${Control.CONTROL_PORT} занят: $e") }
        }
        status(waitingText(ip))
    }

    private fun waitingText(ip: String?) =
        "Ожидание ПК…\nIP: ${ip ?: "нет сети"}  порт ${Control.CONTROL_PORT}"

    /** Everything the PC can change; capture restarts only if it is already running. */
    private fun applyConfig(id: String, width: Int, height: Int, fps: Int, bitrate: Int, hevc: Boolean) {
        val camera = cameras.firstOrNull { it.id == id } ?: return
        val mode = camera.modes.firstOrNull { it.size.width == width && it.size.height == height }
            ?: camera.modes.first()
        val size = mode.size
        val changed = id != activeCameraId || size != activeSize || fps != activeFps ||
                bitrate != activeBitrate || hevc != useHevc

        activeCameraId = id
        activeSize = size
        // Only rates this resolution actually supports; the PC menu offers the same list.
        activeFps = if (fps in mode.fps) fps else mode.fps.lastOrNull { it <= 30 } ?: mode.fps.first()
        activeBitrate = bitrate
        useHevc = hevc
        android.util.Log.i(TAG, "config from PC: $id ${size}@$fps ${bitrate / 1000}kbps hevc=$hevc")

        if (changed && streamer != null) {
            stopStreamer()
            rtpSink?.hevc = hevc
            withSurfaceReady(size) { onStreamStart() }
        } else {
            rtpSink?.hevc = hevc
        }
    }

    private fun onLinked(address: InetAddress, port: Int): StreamInfo? {
        val size = activeSize ?: return null
        activeCameraId ?: return null
        rtpSink?.release()
        val ssrc = Random.nextInt()
        val sink = RtpSink(address, port, ssrc, useHevc)
        rtpSink = sink

        // The PC builds its menus from this, so it must arrive as soon as the link is up -
        // long before any application opens the camera and starts a stream.
        telemetry?.stop()
        telemetry = Telemetry(this) { payload -> sink.sendAux(payload) }.also {
            it.sendCapabilities(cameras)
        }
        runOnUiThread { status("ПК подключён: ${address.hostAddress}\nкамера выключена") }
        return StreamInfo(size.width, size.height, activeFps, activeBitrate, ssrc, sink.localPort,
            useHevc)
    }

    private fun onUnlinked(ip: String?) {
        stopStreamer()
        rtpSink?.release()
        rtpSink = null
        ui.removeCallbacks(ticker)
        wakeScreen()
        if (running) status(waitingText(ip))
    }

    private fun onStreamStart() {
        val size = activeSize ?: return
        val camId = activeCameraId ?: return
        val sink = rtpSink ?: return
        if (streamer != null) return
        // The preview surface has to match the capture size before the session is built:
        // a high-speed session rejects any surface that is not in its own size list.
        if (surfaceSize != size) {
            withSurfaceReady(size) { onStreamStart() }
            return
        }
        if (!launchStreamer(camId, size, sink)) return
        startTelemetry(camId, size, sink)
        lastBytes = 0
        ui.removeCallbacks(ticker)
        ui.postDelayed(ticker, 1000)
        ui.removeCallbacks(blankLater)
        ui.postDelayed(blankLater, SCREEN_ON_MS)
    }

    private fun onStreamStop() {
        stopStreamer()
        ui.removeCallbacks(ticker)
        wakeScreen()
        status("ПК подключён: ${peerLabel ?: "?"}\nкамера выключена")
    }

    private fun setDeviceEis(onDevice: Boolean) {
        if (eisOnDevice == onDevice) return
        eisOnDevice = onDevice
        if (streamer != null) {
            val size = activeSize ?: return
            stopStreamer()
            withSurfaceReady(size) { onStreamStart() }
        }
    }

    private fun startTelemetry(cameraId: String, size: Size, sink: RtpSink) {
        val t = telemetry ?: Telemetry(this) { payload -> sink.sendAux(payload) }.also {
            telemetry = it
            it.sendCapabilities(cameras)
        }
        Cameras.geometryFor(this, cameraId, size.width, size.height)?.let { g ->
            t.sendCameraInfo(g.fx, g.fy, g.cx, g.cy, size.width, size.height, g.orientation,
                cameras.firstOrNull { it.id == cameraId }?.facing ?: -1)
        }
        Cameras.sensorLimits(this, cameraId)?.let { limits ->
            t.sendSensorLimits(limits.isoMin, limits.isoMax, limits.exposureMinNs, limits.exposureMaxNs)
        }
        t.sendCapabilities(cameras)
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
                            "${activeSize?.width}x${activeSize?.height}@$activeFps  $kbps kbps" +
                            (if (useHevc) "  HEVC" else "  H.264") + "\n" +
                            "кадров ${sink.framesSent}\n" + autoState
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
        if (launchStreamer(camId, size, sink)) status("Пишем ${size.width}x${size.height}\n${file.name}")
    }

    private fun launchStreamer(cameraId: String, size: Size, sink: VideoSink): Boolean {
        stopStreamer()
        val surface = surfaceView.holder.surface
        if (surface?.isValid != true) { status("Surface не готов"); return false }
        val s = CameraStreamer(
            context = this,
            cameraId = cameraId,
            size = size,
            fps = activeFps,
            bitrate = activeBitrate,
            sink = sink,
            previewSurface = surface,
            onError = { msg -> status("Ошибка: $msg") },
            onAuto = { text -> autoState = text },
            stabilizeOnDevice = eisOnDevice,
            useHevc = useHevc
        )
        streamer = s
        s.start()
        if (blanked) s.setPreviewEnabled(false)
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
        const val TAG = "MainActivity"
        const val REQ_CAMERA = 1
        const val SCREEN_ON_MS = 15_000L
    }
}
