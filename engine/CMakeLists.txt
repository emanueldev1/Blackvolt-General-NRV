set(BV_ENGINE_SOURCES
  src/api.cpp
  src/core/Log.cpp
  src/gpu/AdapterRanking.cpp
  src/gpu/Adapters.cpp
  src/gpu/D3D12Context.cpp
  src/telemetry/Telemetry.cpp
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

if(WIN32)
  target_link_libraries(blackvolt_core PUBLIC d3d12 dxgi dxguid)
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
