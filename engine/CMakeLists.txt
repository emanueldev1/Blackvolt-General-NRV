include(FetchContent)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# NVIDIA DLSS SDK (headers + NGX static lib). Fetched by scripts/fetch-sdks.ps1 into
# third_party/sdk/dlss; never committed. Without it the engine builds with NGX disabled.
set(BV_DLSS_SDK_DIR "${PROJECT_SOURCE_DIR}/third_party/sdk/dlss" CACHE PATH "NVIDIA DLSS SDK root")
set(BV_HAS_NGX OFF)
if(WIN32 AND EXISTS "${BV_DLSS_SDK_DIR}/include/nvsdk_ngx.h")
  set(BV_HAS_NGX ON)
endif()
message(STATUS "Blackvolt: NGX support ${BV_HAS_NGX} (SDK: ${BV_DLSS_SDK_DIR})")

# FFmpeg: vcpkg on Windows (find_package wrapper), pkg-config elsewhere.
find_package(FFMPEG QUIET)
if(FFMPEG_FOUND)
  add_library(bv_ffmpeg INTERFACE)
  target_include_directories(bv_ffmpeg INTERFACE ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(bv_ffmpeg INTERFACE ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(bv_ffmpeg INTERFACE ${FFMPEG_LIBRARIES})
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(BV_FF REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswresample libswscale)
  add_library(bv_ffmpeg INTERFACE)
  target_link_libraries(bv_ffmpeg INTERFACE PkgConfig::BV_FF)
endif()

set(BV_ENGINE_SOURCES
  src/api.cpp
  src/player_api.cpp
  src/audio/WasapiSink.cpp
  src/render/D3D12Presenter.cpp
  src/core/Engine.cpp
  src/core/Log.cpp
  src/core/TimerResolution.cpp
  src/media/AudioDecoder.cpp
  src/media/Demuxer.cpp
  src/media/VideoDecoder.cpp
  src/player/Player.cpp
  src/ngx/NgxRuntime.cpp
  src/ngx/VideoSuperResolution.cpp
  src/ngx/TrueHdr.cpp
  src/runtime/Catalog.cpp
  src/runtime/Manifest.cpp
  src/runtime/Sha256.cpp
  src/gpu/AdapterRanking.cpp
  src/gpu/Adapters.cpp
  src/gpu/D3D12Context.cpp
  src/telemetry/Telemetry.cpp
  src/remote/ConsoleProfile.cpp
  src/remote/QtSettingsCodec.cpp
  src/remote/ChiakiImport.cpp
  src/remote/VeyraImport.cpp
  src/remote/Protect.cpp
  src/remote/ConsoleStore.cpp
)

# Platform-independent core, linked by the DLL and by the unit tests.
add_library(blackvolt_core STATIC ${BV_ENGINE_SOURCES})
target_include_directories(blackvolt_core
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(blackvolt_core PUBLIC BV_ENGINE_BUILD)
set_target_properties(blackvolt_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  target_compile_options(blackvolt_core PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  target_compile_definitions(blackvolt_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(blackvolt_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(blackvolt_core PUBLIC nlohmann_json::nlohmann_json bv_ffmpeg)

if(WIN32)
  target_link_libraries(blackvolt_core PUBLIC d3d12 dxgi dxguid d3dcompiler dwmapi ole32 crypt32 advapi32 winmm iphlpapi ws2_32)
endif()

if(BV_HAS_NGX)
  target_compile_definitions(blackvolt_core PUBLIC BV_HAS_NGX)
  target_include_directories(blackvolt_core PRIVATE "${BV_DLSS_SDK_DIR}/include")
  target_link_libraries(blackvolt_core PUBLIC
    debug "${BV_DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib"
    optimized "${BV_DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
endif()

if(BV_ENABLE_REMOTEPLAY)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON) # chiaki-lib ends up inside the engine DLL / .so
  include(${PROJECT_SOURCE_DIR}/cmake/RemotePlay.cmake)
  target_sources(blackvolt_core PRIVATE
    src/remote/ChiakiRuntime.cpp
    src/remote/Discovery.cpp
    src/remote/Registration.cpp
    src/remote/StreamSession.cpp
    src/remote/SdlGamepad.cpp
    src/remote_api.cpp)
  target_compile_definitions(blackvolt_core PUBLIC BV_HAS_REMOTEPLAY)
  target_link_libraries(blackvolt_core PUBLIC chiaki-lib)
  find_package(SDL3 CONFIG QUIET)
  if(SDL3_FOUND)
    target_compile_definitions(blackvolt_core PRIVATE BV_HAS_SDL3)
    target_link_libraries(blackvolt_core PUBLIC SDL3::SDL3)
    message(STATUS "Blackvolt: mandos vía SDL3 ${SDL3_VERSION}")
  else()
    message(WARNING "Blackvolt: SDL3 no encontrado, el streaming no leerá mandos físicos")
  endif()
endif()

add_library(blackvolt_engine SHARED src/dll_main.cpp)
target_link_libraries(blackvolt_engine PRIVATE blackvolt_core)
if(WIN32)
  # Export every bv_* symbol from the static core through the DLL.
  set_target_properties(blackvolt_engine PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
  target_link_options(blackvolt_engine PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:blackvolt_core>")
else()
  target_link_options(blackvolt_engine PRIVATE "-Wl,--whole-archive" $<TARGET_FILE:blackvolt_core> "-Wl,--no-whole-archive")
endif()
