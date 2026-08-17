# AndroidWebCam

An Android phone as a Windows virtual webcam over Wi-Fi. H.264, 1080p30, hardware encoding on the
phone and hardware decoding on the PC, RTP/UDP, minimal latency.

```
Camera2 ──surface──> MediaCodec(H.264 HW) ──> RTP/UDP ──> WinSock
                                                            │
                                          MF H.264 decoder (DXVA2/D3D11)
                                                            │
                                        shared memory (Global\AWC_Frames_v1)
                                                            │
                                    awc-source.dll inside Frame Server
                                                            │
                                        Zoom / Meet / Discord / OBS / ...
```

## Layout

| Path | What it is |
|---|---|
| `android-client/` | Phone app (Kotlin, no external dependencies) |
| `windows-client/src/` | RTP receive, depacketize, decode, publish frames |
| `windows-client/src/source/` | Virtual camera COM DLL (loaded by the Frame Server service) |

## Requirements

| Component | Minimum | Why |
|---|---|---|
| Windows 11 | 22H2 | `IMFVirtualCamera` did not exist earlier — nothing to register a camera with |
| Windows SDK | one that ships `mfvirtualcamera.h` (10.0.22621.0+) | same API |
| Compiler | MSVC (VS 2022 or Build Tools), C++17 | Media Foundation, COM |
| CMake | 3.21 | ships with VS |
| Android Studio / Android SDK | platform API 31+ | `minSdk 31` |
| JDK | 17 | project's `sourceCompatibility` |
| Gradle | wrapper included in the repo | — |
| Phone | Android 12+ (API 31), hardware H.264 encoder | Camera2 + MediaCodec |

Gyro stabilization additionally requires the phone's camera to report
`SENSOR_INFO_TIMESTAMP_SOURCE == REALTIME` — without a clock shared with the sensors there is no way
to match a rotation to a frame (see "Gyro stabilization"). If your device does not, just leave the
mode off; everything else works.

## Build

### Android

```bash
cd android-client && .\gradlew.bat assembleDebug
```

`gradlew.bat` picks up the JDK from Android Studio when `JAVA_HOME` is unset. Gradle reads the SDK
path from `local.properties` (local file, not in the repo):

```
sdk.dir=C:/Users/<you>/AppData/Local/Android/Sdk
```

Install:

```bash
adb install -r android-client\app\build\outputs\apk\debug\app-debug.apk
```

Some vendor ROMs (MIUI/HyperOS among them) block `adb install` with
`INSTALL_FAILED_USER_RESTRICTED`. Workaround — install from the phone's shell:

```bash
adb push app-debug.apk /data/local/tmp/awc.apk && adb shell pm install -r -t /data/local/tmp/awc.apk
```

Starting without touching the screen (handy while debugging):

```bash
adb shell am start -n com.androidwebcam/.MainActivity --ez autostart true
```

`--ei mode 1` records to a file instead of streaming.

### Windows

```bash
cd windows-client && cmake -S . -B build -G "Visual Studio 17 2022" -A x64 && cmake --build build --config Release
```

`CMakeLists.txt` pins `CMAKE_SYSTEM_VERSION` — put the SDK version you actually have installed
there or generation fails. The CMake bundled with VS lives at
`<VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.

Registering the virtual camera source — **once, from an elevated PowerShell**:

```bash
powershell -ExecutionPolicy Bypass -File windows-client\install.ps1
```

The script copies `awc-source.dll` into `C:\Program Files\AndroidWebCam\` (the Frame Server service
runs as LOCAL SERVICE and cannot read files from a user profile) and registers the CLSID under
HKLM. `uninstall.ps1` reverts it.

## Running

1. On the PC, start `windows-client\build\Release\awc-client.exe` — there is no window, the app
   lives in the tray. Icon color: gray — no phone found, yellow — phone connected, camera off,
   green — streaming. Click the icon for status and "Exit".
2. On the phone: open AndroidWebCam and press Start.
3. The PC finds the phone on the local network by itself and keeps the link alive.
4. The "AndroidWebCam" camera is available to any app — Zoom, Discord, Teams, browsers, OBS, the
   stock Windows Camera app. The phone's camera turns on only while an app actually holds the
   device open, and goes dark when it lets go.

Flags: `--ip <address>` skips discovery, `--port` picks another RTP port, `--remove` unregisters,
`--test` is a console diagnostic mode.

### All controls live on the PC

The tray menu owns capture: **Camera**, **Resolution**, **Frame rate**, **Bitrate**, **Codec**, plus
the processing toggles. The phone app is a status display with a Start button; it holds no settings
of its own.

The phone reports what it can do (cameras with their labels and 16:9 sizes, and the frame rates the
sensor advertises) in a `CAPABILITIES` message repeated with the rest of the telemetry, so the menus
list what actually exists rather than a hardcoded guess. Picks go back as `SET_CONFIG`. After
sending one, the PC deliberately drops the link: the reconnect brings a fresh `HELLO_ACK`, and the
decoder is then built from what the phone really applied instead of what we asked for.

Frame rates on offer come from `CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES` — on this device
12/15/24/30 (see "Frame rate is capped by the sensor"). Choices are remembered in
`HKCU\Software\AndroidWebCam`.

### The phone screen turns itself off

While streaming, the phone blanks after 15 seconds: window brightness to 0, black UI, and the
preview stream is dropped from the repeating request so the camera stops producing buffers nobody
looks at. Touching the screen brings it back for another 15 seconds. Verified through the system:
the window reports `sbrt=0.0` and the display manager `screenBrightnessOverride=0.0`.

The preview is toggled by re-issuing the repeating request without that target — the session is not
reconfigured, so there is no hiccup in the stream.

Note what this is not: the display is driven to zero, not switched off, because capture is tied to a
foreground activity. A genuinely off screen with the stream alive needs a foreground camera service,
which is a larger change. On AMOLED, a black frame at zero brightness costs almost nothing anyway.

### Gyro stabilization

Toggled by **Gyro stabilization** in the tray menu (which also shows the current correction in
degrees). The phone captures with headroom — 2560x1440 by default — and the PC stabilizes and
downscales to 1080p in a single pass.

How it works:

1. The phone streams gyro samples (200 Hz) into the same UDP socket as the video, so no extra
   firewall hole is needed. Camera intrinsics travel with them, and every frame carries a sensor
   timestamp, its exposure and the rolling shutter skew.
2. This only works because frame timestamps and gyro events share one clock
   (`SENSOR_INFO_TIMESTAMP_SOURCE == REALTIME`).
3. The PC integrates angular velocity into a quaternion, smooths the trajectory (EMA, τ = 500 ms),
   takes the residual rotation and applies it as the homography `K·Rᵀ·K_out⁻¹`.
4. **The correction is computed per row, not per frame.** On a typical sensor the rolling shutter
   skew is comparable to the frame duration — top and bottom are captured nearly a frame apart.
   Under fast small shake the frame therefore does not shift, it bends, and a single rotation per
   frame only emphasizes that deformation. Homographies are built for bands of rows and
   interpolated between them.
5. The correction is clamped to the crop margin (10% by default): without the clamp, sampling runs
   past the frame edge and the borders smear, which reads as distortion.
6. EIS and OIS on the phone are forced off in this mode — otherwise frames arrive already deformed
   by someone else's stabilization and the gyro data no longer describes them.

This adds almost no latency: while a frame is encoded, travels the network and gets decoded, the
gyro runs ahead by the ~120 ms the smoothing needs.

The meaningful comparison is not "with and without stabilization" but against the phone's own EIS,
which comes back when this mode is off. Measure with `--analyze`: residual frame shake and row
shear ("jello") in px rms. In our runs gyro stabilization improved both by roughly 3×; what remains
is the high-frequency part — a 200 Hz gyro corrects up to about 100 Hz. On a still phone the
correction is zero and introduces no artifacts.

**The second defect is blur, not geometry.** In a dim room auto exposure drifts toward 25 ms out of
a 33 ms frame and any motion smears within the frame. Stabilization cannot help here by
construction — it fixes geometry, not blur. Use "Short exposure" for that.

### Codec: H.264 or HEVC

Picked by the codec spinner in the phone app (`--ei codec 1` selects HEVC when starting from
adb). The phone uses `c2.qti.hevc.encoder`, the PC decodes with whatever HEVC MFT is installed —
check yours with `awc-client.exe --decoders`.

HEVC needs its own RTP payload format (RFC 7798, not 6184): NAL headers are two bytes, the
fragmentation type is 49 and aggregation 48, and the parameter sets arrive as VPS+SPS+PPS packed
into a single `csd-0` rather than split across `csd-0`/`csd-1`. IRAP pictures are NAL types 16–21,
where H.264 has a single type 5.

Where it helps: at a bitrate that is already generous, both codecs look the same and HEVC only
costs the phone more heat. It pays off where bits are scarce. Measured on the same scene at 1080p30:

| Bitrate | H.264 detail | HEVC detail |
|---|---|---|
| 8 Mbps | 2.4 | 2.3 (no difference — both transparent here) |
| 2 Mbps | 1.9 | **2.4** |

("detail" is the mean-gradient metric `--analyze` prints; the clips are seconds apart on a static
scene, so treat it as indicative, not a codec benchmark.)

### 120 and 240 fps

Offered per resolution, because that is how the hardware offers them: on this device 120 fps
exists up to 1080p and 240 fps only at 720p and below, while the ultra-wide and macro sensors have
no high-speed modes at all. The **Resolution** menu shows each entry's ceiling, and the
**Frame rate** menu lists only what the selected resolution can hold. The phone additionally checks
that an encoder can take that size at that rate before offering it.

Above 30 fps the phone switches to a constrained high-speed session, which is a different API
(`createHighSpeedRequestList` + `setRepeatingBurst`) and refuses manual sensor control, so the
short-exposure controller stands down — at 120 fps the exposure is at most 8 ms anyway, which was
the point of that mode.

The PC does not throw the extra frames away: it **averages** them down to ~30 fps out (4 frames at
120, 8 at 240). Averaging four 8 ms exposures gives back the light a short exposure gave up and
halves the noise, and because stabilized frames are warped onto a common orientation before
stacking, camera shake does not smear the result.

**The preview surface must be left out of a high-speed session.** With both the encoder and the
preview attached, this HAL splits the frame batch between the two outputs and hands the encoder an
empty buffer for every second frame. It arrives as a perfectly black frame, and once averaged with
the good ones the picture comes out dark and green (chroma 64 instead of the neutral 128 — the
mixed-in zeros pull it there). The same alternating pattern showed up decoding the raw dump with
ffmpeg, which is what ruled out our own decoder. Above 30 fps the session therefore carries the
encoder surface alone, and the phone shows no preview in those modes.

Two more things this cost, both measured:

- **Receive and decode had to be separated.** Decoding used to run inside the socket-draining loop.
  At 30 fps there was slack; at 120 there was none, and the socket overflowed — 2286 packets lost in
  eight seconds, even with stabilization off, so it was not the warp. Assembled frames are now
  queued during the drain and decoded after it. Same test afterwards: **0 lost, 0 dropped**.
- **Bitrate has to be sane.** 720p240 at 40 Mbps made the encoder burst to 78 Mbps and drowned the
  link (8193 packets lost). At 12 Mbps the same mode runs clean.

In practice the phone delivers ~95–120 fps rather than a solid 120 once it is warm; 240 fps is
selectable and runs, but expect the sensor and encoder to fall short of the nominal rate.

**These modes cost latency, inherently.** An output frame cannot leave before its whole group has
arrived (33 ms at 120 fps), every frame is warped individually so the CPU does four times the work
per output frame, and the link carries four times the packets. The frame-ready wait is now scaled to
the frame rate rather than a fixed 250 ms, which removes the worst spikes, but the group delay
remains. If latency matters more than blur and noise, stay at 30 fps.

### Frame rate is capped by the sensor, not the codec

Worth knowing before planning anything around 60 fps: on this device
`getOutputMinFrameDuration` is **33333 µs at every size** — 1080p, 1440p, 720p alike — so a regular
capture session cannot exceed 30 fps no matter what is requested, and `CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES`
offers nothing above `[30,30]`. Both hardware encoders happily do 1080p60 and 1080p120; the camera
is the limit.

Above 30 fps there is only `createConstrainedHighSpeedCaptureSession`, and it offers **120 and 240
fps only** — no 60. That session also restricts 3A: manual exposure and EIS are off the table, so it
would replace the short-exposure controller rather than complement it (at 120 fps the exposure is
implicitly ≤8.3 ms anyway).

### Horizon leveling

The **Level horizon** checkbox in the tray menu (its label shows the current tilt).

**A gyroscope fundamentally cannot do this.** A gyro measures angular velocity: integrate it and
you get rotation relative to wherever you started, plus accumulating drift — it does not know which
way is down. Only gravity gives an absolute vertical, so the phone also sends `TYPE_GRAVITY` (an
already filtered vector, free of motion jerk) at 25 Hz. The gyro still handles fast stabilization;
gravity anchors roll.

Roll is computed as `atan2(gx, gy)` in camera axes from heavily smoothed gravity (τ ≈ 1 s: the
horizon does not move, everything fast here is shake), then mixed into the desired frame
orientation. It works independently of gyro stabilization.

Two subtleties, both important:

- **The 180° ambiguity.** Which way the frame's "down" points depends on how the phone is mounted,
  and the raw angle can come out half a turn off. Tuning the sign for one particular mount is the
  wrong fix — instead the angle is folded into ±90°, because a horizon leveler should never rotate
  by half a turn by definition. As a bonus the mode works with any mounting.
- **Rotation needs headroom in the corners.** Tilt is limited by what the crop allows, otherwise
  black wedges appear in the corners. A 10% crop levels roughly up to 10°; if the margin is shared
  with stabilization, less angle is available.
- If the camera points nearly straight up or down there is no horizon (gravity along the optical
  axis) — no rotation is applied in that case.

Checking it without a phone: `awc-client.exe --leveltest` builds the frame a camera tilted by 5°
would see, feeds the matching gravity vector and measures the result.

Metering on the detected face was tried and removed: exposing for a small, moving region made the
brightness lag visibly behind the scene, and it fought the exposure cap (a face in a dim room asks
for more light than ISO 2000 can give at a short exposure, so the controller kept walking the
exposure up — undoing the mode). Metering is whole-frame.

### Short exposure

The **Short exposure** checkbox in the tray menu (its label shows the current exposure and ISO).

Exposure is pinned to 10 ms and brightness is recovered with gain. The ISO controller lives on the
PC: decoded luma is already at hand there, so the phone does not need an extra camera stream just
to meter (which might not fit the session configuration limits anyway), and the tuning can change
without rebuilding the APK. The phone receives `SET_EXPOSURE` and applies `CONTROL_AE_MODE_OFF` +
`SENSOR_EXPOSURE_TIME` + `SENSOR_SENSITIVITY`.

**Why 10 ms.** Under 50 Hz mains, light pulsates at 100 Hz. An exposure that is not a whole number
of pulsation periods samples a different phase in different rows and produces banding across the
frame. 10 ms is exactly one period: 2.5× less blur, no bands. That is also why the exposure ladder
steps in 10 ms increments (10 → 20 → 30) while the picture still needs light; the controller only
goes below 10 ms once gain is already at its minimum, i.e. in bright light where flicker is
usually not an issue.

**On 60 Hz mains** the period is 8.33 ms — change `kMainsPeriodNs` in
[ExposureControl.cpp](windows-client/src/ExposureControl.cpp) or you will get banding.

The ladder stops at **20 ms** even if the picture stays dark: 33 ms would bring the blur straight
back, which is the whole point of the mode. This bites together with face metering — exposing for a
face in a dim room needed more light than ISO 2000 could supply at 10 ms, so the controller walked
up to the 20 ms cap and parked at maximum gain. Prefer a brighter picture over a sharp one? Raise
`kMaxExposureNs`.

The price is noise: in a dark room ISO climbs past a thousand. With enough light the mode is
pointless — auto exposure picks a short exposure on its own.

#### Axis check: `--gyrocheck`

```bash
awc-client.exe --gyrocheck --ip <phone address>
```

Wave the phone in your hand for 15 seconds — along all three axes, including a roll around the
lens. The tool measures frame displacement from the picture itself (normalized correlation of row
and column profiles), integrates the **unmapped** gyro along all three axes and prints a
correlation matrix for time offsets from −60 to +60 ms. In other words it does not verify the axis
mapping, it derives it from the data: horizontal shift ← `gyro x`, vertical ← `gyro y`, roll ←
`gyro z`, with signs and scale in px/rad. A good run shows |correlation| above ~0.8 on all three.

**Roll must be measured separately.** Two shift axes alone determine the mapping ambiguously: it
has a mirror twin with the same pair of signs, and only the sign of rotation tells them apart. A
sign error does not "slightly degrade" the picture — the correction starts doubling the shake.

The "best time offset" it reports, around +20…30 ms, is not a sync error but physics: a frame
timestamp marks the start of the first row's exposure, while the shift is measured over the whole
frame, whose temporal center sits `skew/2 + exposure/2` later. The production path accounts for
exactly that.

### Mirroring

In the tray menu: **Mirror horizontally** and **Mirror vertically**, independent, both together
give a 180° rotation. Flipping happens on the PC in a single pass over the NV12 frame (luma by
rows, chroma by U/V pairs) and survives restarts — the settings live in
`HKCU\Software\AndroidWebCam`.

### Sensor selection

On most phones `CameraManager.getCameraIdList()` returns only the two "main" cameras; the
ultra-wide, macro and tele hide behind IDs the system does not enumerate but which can still be
queried and **opened** with a plain `openCamera`. The app therefore probes IDs 0–9, drops the ones
with no 16:9 outputs suitable for MediaCodec, and collapses duplicates of the same sensor (same
facing and focal length — vendors expose the main camera several times over). The list shows each
discovered sensor's focal length and its maximum 16:9 resolution.

The sensor can be switched **on the fly**, mid-stream: capture restarts while the PC link and the
RTP socket stay up. There is no automatic switching between sensors — a main camera's zoom range
usually starts at 1.0 and never goes below, so the choice is manual.

For debugging, the camera can be picked at startup or switched on a running app (the activity is
`singleTop`, the intent arrives in `onNewIntent`):

```bash
adb shell am start -n com.androidwebcam/.MainActivity --ez autostart true --es cameraId 3 --ei width 1920
```

### Image automation (3A)

Every mode goes into the repeating request and is recomputed by the HAL continuously, so changing
light is handled on its own. Each mode is checked against the supported list — hidden sensors lack
some of them (a macro camera without autofocus reports only `OFF`):

- exposure `CONTROL_AE_MODE_ON` with an explicit `AE_LOCK=false` and zero compensation;
- white balance `CONTROL_AWB_MODE_AUTO` with `AWB_LOCK=false`;
- focus — the best available of `CONTINUOUS_VIDEO` → `CONTINUOUS_PICTURE` → `AUTO`;
- antibanding `AUTO` — otherwise mains flicker shows up as bands;
- AE/AWB/AF metering regions stretched over the whole frame instead of the HAL defaults;
- ISP blocks in `FAST`: noise reduction, edge enhancement, tone mapping, hot pixel correction,
  shading, chromatic aberration — some HALs keep these off until asked;
- stabilization: the best available is picked — `PREVIEW_STABILIZATION` (API 33+, a tighter EIS
  meant for streaming), otherwise the classic `ON`; plus optical stabilization if the sensor
  advertises it.

Doing stabilization on the phone rather than the PC is deliberate: the phone's EIS runs on gyro
data, while on the PC it would have to be estimated from the picture — more latency and more crop
for a noticeably worse result. PC-side gyro stabilization is a separate mode, and it replaces the
phone's EIS.

Convergence state is visible in the phone's status line:
`фокус: наводится · экспозиция: готово · ББ: готово · ISO 2006` (the phone UI is in Russian).

## Stage-by-stage verification

| Stage | Command | Expected |
|---|---|---|
| 1. Encoding | file (mp4) mode, then `adb pull /sdcard/Android/data/com.androidwebcam/files/<file>.mp4` | `ffprobe`: H.264 1920x1080 ~30 fps; `ffmpeg -i file -f null -` with no errors |
| 2–3. Network + decode | `awc-client.exe --test --seconds 10 --dump out.h264 --frame first.nv12` | kbps/fps stats, `lost 0`, `decoded > 0` |
| 4. Camera source | `awc-client.exe --selftest build\Release\awc-source.dll` | all QI succeed, `MENewStream`, `samples delivered > 0` |
| 5. Camera in the system | `awc-client.exe --list`, then `--capture --frame vcam.nv12` | "AndroidWebCam" in the list, `samples read > 0` |
| 6. Any application | `ffmpeg -f dshow -i "video=AndroidWebCam (Windows Virtual Camera)" -t 10 out.mp4` | 1920x1080, ~30 fps |

The device name in step 6 is localized by Windows — take the exact string from `--list`.

Diagnostic modes: `--list` (MF devices), `--capture` (open the camera like a normal app),
`--selftest <dll>` (exercise the source COM object directly, without the registry or the service),
`--test` (receive and decode without the virtual camera). Logs: `C:\ProgramData\AndroidWebCam\`.

Inspecting a `--frame` dump:

```bash
ffmpeg -f rawvideo -pix_fmt nv12 -s 1920x1080 -i first.nv12 -frames:v 1 first.png
```

## Bitrate

In the phone app: 4 / 6 / 8 / 12 / 16 / 20 / 25 / 30 / 40 Mbps, 20 by default.

The requested bitrate is delivered accurately up to a few tens of Mbps; the ceiling is not Wi-Fi
but the UDP path — 100 Mbps means ~9000 packets/s at MTU 1400. There is no practical point above
~20 Mbps: the picture saturates visually around 8–12, and Zoom/Meet/Discord re-encode down to
1–3 Mbps on their side anyway. High bitrate only shows locally — in OBS, while recording, or in
preview.

## Protocol

Control — UDP, port **45001** (the phone listens), big-endian, header `"AWC1"` + type.

| Type | Direction | Payload |
|---|---|---|
| `0x01` HELLO | PC → phone | u16 rtpPort — link is up, camera still off |
| `0x02` HELLO_ACK | phone → PC | u16 w, u16 h, u8 fps, u32 bitrate, u32 ssrc, u16 rtpSourcePort |
| `0x03` KEEPALIVE | PC → phone | — (at least every 1 s; the phone drops the link after 5 s of silence) |
| `0x04` REQUEST_IDR | PC → phone | — |
| `0x05` SET_BITRATE | PC → phone | u32 bps |
| `0x06` BYE | PC → phone | — |
| `0x07` STREAM_START | PC → phone | — an app opened the camera, start capturing |
| `0x08` STREAM_STOP | PC → phone | — it let go, shut the camera down, keep the link |
| `0x10` DISCOVER | PC → subnet hosts | — |
| `0x11` FOUND | phone → PC | u16 controlPort, u8 len, name |
| `0x12` PUNCH | PC → phone:rtpSourcePort | — |

Video — RTP/UDP, payload type 96, 90 kHz clock, RFC 6184 (single NAL + FU-A), MTU 1400.
SPS/PPS are sent before every IDR and an IDR goes out once a second, so a receiver can join at any
moment.

**PUNCH** works around Windows Firewall without admin rights: the phone reports its RTP source
port, the PC sends an empty datagram there from its own receiving socket and opens a stateful UDP
entry, after which the incoming stream gets through. Repeated with every keepalive.

**DISCOVER is sent as unicast to every address in the local subnet**, not as a broadcast: the phone
answers a broadcast from its own address, and the firewall treats such a reply as unsolicited and
drops it. A probe to each host creates the matching stateful entry.

## Tuning it for your setup

| What | Where |
|---|---|
| Capture resolution, bitrate list, default | `DEFAULT_SIZES`, `BITRATES`, `DEFAULT_BITRATE` in [MainActivity.kt](android-client/app/src/main/java/com/androidwebcam/MainActivity.kt) |
| Virtual camera output resolution | `kMaxWidth` / `kMaxHeight` in [SharedFrames.h](windows-client/src/SharedFrames.h) |
| Crop headroom for stabilization and leveling | `crop` in [main.cpp](windows-client/src/main.cpp) |
| Number of rolling shutter bands | `kBands` in the same file |
| Mains period for short exposure (50/60 Hz) | `kMainsPeriodNs` in [ExposureControl.cpp](windows-client/src/ExposureControl.cpp) |
| Gyro sample rate | `RATE_US` in [Telemetry.kt](android-client/app/src/main/java/com/androidwebcam/Telemetry.kt) |
| Control port, MTU | `CONTROL_PORT` in [Control.kt](android-client/app/src/main/java/com/androidwebcam/Control.kt), `MTU` in [RtpSink.kt](android-client/app/src/main/java/com/androidwebcam/RtpSink.kt), [Protocol.h](windows-client/src/Protocol.h) |
| Camera name and GUID in the system | [guid.cpp](windows-client/src/source/guid.cpp), `install.ps1` |
| Windows SDK version used for the build | `CMAKE_SYSTEM_VERSION` in [CMakeLists.txt](windows-client/CMakeLists.txt) |
| `applicationId`, `minSdk`, `compileSdk` | [app/build.gradle.kts](android-client/app/build.gradle.kts) |

Change capture and output resolution together: the phone captures with headroom and the PC
stabilizes and downscales to the output size in one pass. If you do not need stabilization, capture
straight at the output resolution — noticeably less load on the phone and on the link.

## Gotchas

- **VPN.** With a VPN you need split tunneling: otherwise the VPN claims the local subnet route
  (metric 0 against Wi-Fi's 256) and the phone becomes unreachable, not even pingable. Either
  exclude the local network from the tunnel or turn the VPN off.
- **ROM restrictions.** `adb install` may be blocked unless "Install via USB" is enabled; `pm grant`
  and `adb shell input` need "USB debugging (Security settings)". The install workaround is
  `pm install` from the shell.
- **DLL in use.** Frame Server keeps the DLL loaded, so `install.ps1` stops the `FrameServer` and
  `FrameServerMonitor` services before replacing the file — they are demand-start and come back on
  their own.

### Clocks: encoder PTS and sensors live in different time bases

`MediaCodec` stamps output buffers with **monotonic** time (`System.nanoTime`), while camera frame
timestamps and sensor events use **boottime** (`elapsedRealtimeNanos`). The difference equals the
time the phone spent in deep sleep and can reach hours.

The symptom is insidious: nothing crashes and nothing is logged, but frame intervals and gyro
samples never overlap, orientation interpolation pins to the edge of the history, the correction
degenerates into a constant — and stabilization quietly becomes a static crop.

The fix is a constant offset: `boottime = pts + (elapsedRealtimeNanos − nanoTime)`, sampled once
from two adjacent calls (accurate to microseconds and stable for the session, since deep sleep
cannot happen while streaming).

### What Frame Server demands from a source (all of it found the hard way)

Every item below caused a total failure, and none of them is diagnosable from an error code — only
from the log of failed `QueryInterface` calls in `source.log`:

- **`IMFActivate`.** The object behind the CLSID must be an activator: the frame server asks for
  `IMFActivate` and gets the media source through `ActivateObject`. Without it
  `IMFVirtualCamera::Start` returns `E_NOINTERFACE`.
- **`IMFAttributes`.** The source itself must be an attribute store (delegated to an inner one).
- **Sensor profile.** Without `MF_DEVICEMFT_SENSORPROFILE_COLLECTION` in the source attributes,
  `ReadSample` fails with `MF_E_HW_MFT_FAILED_START_STREAMING` — and the source's `Start` is never
  called at all.
- **System-clock timestamps.** A live source must stamp `MFGetSystemTime()`, not time from zero —
  otherwise samples are delivered, `QueueEvent` returns `S_OK`, but the application receives
  nothing and requests stop.
- **Pace by frame arrival, not by a timer.** A frame-length timer beats against the real frame
  interval and every other tick is skipped: 20 fps instead of 30.
