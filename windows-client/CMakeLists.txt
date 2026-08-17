cmake_minimum_required(VERSION 3.21)
project(AndroidWebCam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_SYSTEM_VERSION 10.0.26100.0)

set(AWC_DEFS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# Without this MSVC reads sources in the system code page and mangles any
# non-ASCII literal.
add_compile_options(/utf-8)

# Controller app: receives RTP, decodes, publishes frames, registers the camera.
add_executable(awc-client WIN32
    src/main.cpp
    src/Session.cpp
    src/Depacketizer.cpp
    src/H264Decoder.cpp
    src/ProducerChannel.cpp
    src/Stabilizer.cpp
    src/Warp.cpp
    src/ExposureControl.cpp
    src/Log.cpp
)
target_compile_definitions(awc-client PRIVATE ${AWC_DEFS})
target_link_libraries(awc-client PRIVATE
    ws2_32 mfplat mfuuid mf mfreadwrite mfsensorgroup d3d11 dxgi ole32 strmiids advapi32
)

# Virtual camera media source, loaded by the Windows Frame Server service.
add_library(awc-source SHARED
    src/source/dllmain.cpp
    src/source/guid.cpp
    src/source/MediaSource.cpp
    src/source/FrameChannel.cpp
    src/Log.cpp
)
target_compile_definitions(awc-source PRIVATE ${AWC_DEFS})
target_link_libraries(awc-source PRIVATE mfplat mfuuid mfsensorgroup ole32 advapi32)
target_link_options(awc-source PRIVATE
    "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/source/awc-source.def"
)
set_target_properties(awc-source PROPERTIES PREFIX "" OUTPUT_NAME "awc-source")
