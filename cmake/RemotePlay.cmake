# PlayStation Remote Play: chiaki-ng's protocol library (AGPL-3.0 + OpenSSL exception),
# pinned to a reviewed commit plus Blackvolt's small patches in third_party/patches.
# A local checkout can be used with -DFETCHCONTENT_SOURCE_DIR_CHIAKI_NG=<path> (already patched).
include(FetchContent)

set(BV_CHIAKI_COMMIT 43728af2e639fecb8a7af1cd68c29820a40de973)
file(GLOB BV_CHIAKI_PATCHES "${PROJECT_SOURCE_DIR}/third_party/patches/chiaki-ng/*.patch")
list(SORT BV_CHIAKI_PATCHES)

find_package(Git REQUIRED)
FetchContent_Declare(chiaki_ng
  GIT_REPOSITORY https://github.com/streetpea/chiaki-ng.git
  GIT_TAG ${BV_CHIAKI_COMMIT}
  GIT_SUBMODULES third-party/curl third-party/gf-complete third-party/jerasure third-party/nanopb
  GIT_SUBMODULES_RECURSE OFF
  GIT_PROGRESS OFF
  # Keep upstream line endings: the patches are LF and Windows runners default to autocrlf=true.
  GIT_CONFIG core.autocrlf=false core.eol=lf
  UPDATE_DISCONNECTED ON
  PATCH_COMMAND "${GIT_EXECUTABLE}" -c core.autocrlf=false apply --ignore-whitespace --whitespace=nowarn ${BV_CHIAKI_PATCHES}
  EXCLUDE_FROM_ALL)

# Python runs nanopb's generator (and protoc via grpcio-tools when no protoc is installed).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(PYTHON_EXECUTABLE "${Python3_EXECUTABLE}" CACHE FILEPATH "" FORCE)
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/compat") # FindPythonInterp shim for CMake 4

# Only the protocol library: no GUI, CLI, decoders or extras.
foreach(opt GUI CLI TESTS ANDROID BOREALIS STEAM_SHORTCUT)
  set(CHIAKI_ENABLE_${opt} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(opt FFMPEG_DECODER PI_DECODER SETSU STEAMDECK_NATIVE SPEEX)
  set(CHIAKI_ENABLE_${opt} OFF CACHE STRING "" FORCE)
endforeach()
set(CHIAKI_ENABLE_RUDP ON CACHE STRING "" FORCE) # remote play over the internet (PSN)
set(CHIAKI_LIB_ENABLE_OPUS ON CACHE BOOL "" FORCE)
set(CHIAKI_LIB_ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(CHIAKI_USE_SYSTEM_NANOPB OFF CACHE STRING "" FORCE)
set(CHIAKI_USE_SYSTEM_JERASURE OFF CACHE STRING "" FORCE)
set(CHIAKI_USE_SYSTEM_CURL OFF CACHE STRING "" FORCE)

# json-c, miniupnpc and libevent come from vcpkg (Windows) or pkg-config (Linux).
set(CHIAKI_DEPS_FROM_PARENT ON CACHE BOOL "" FORCE)
if(WIN32)
  find_package(json-c CONFIG REQUIRED)
  find_package(miniupnpc CONFIG REQUIRED)
  find_package(Libevent CONFIG REQUIRED)
  set(CHIAKI_PARENT_DEPS json-c::json-c miniupnpc::miniupnpc libevent::core libevent::extra
      CACHE STRING "" FORCE)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(BV_JSONC REQUIRED IMPORTED_TARGET GLOBAL json-c)
  pkg_check_modules(BV_UPNP REQUIRED IMPORTED_TARGET GLOBAL miniupnpc)
  pkg_check_modules(BV_EVENT REQUIRED IMPORTED_TARGET GLOBAL libevent)
  set(CHIAKI_PARENT_DEPS PkgConfig::BV_JSONC PkgConfig::BV_UPNP PkgConfig::BV_EVENT CACHE STRING "" FORCE)
endif()

enable_language(C)
FetchContent_MakeAvailable(chiaki_ng)

if(NOT TARGET chiaki-lib)
  message(FATAL_ERROR "chiaki-ng no creó el target chiaki-lib")
endif()
if(MSVC)
  # Third-party C: keep its warnings out of our logs.
  target_compile_options(chiaki-lib PRIVATE /W0)
endif()
