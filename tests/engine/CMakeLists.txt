add_executable(blackvolt_engine_tests
  main.cpp
  test_rolling_stats.cpp
  test_adapter_ranking.cpp
  test_api.cpp
  test_runtime.cpp
  test_player.cpp
  test_remote_import.cpp
  test_enhance.cpp
  test_interpolation.cpp
  test_denoise.cpp
  test_nis.cpp
  test_timestretch.cpp
  test_postfx.cpp
  test_capture.cpp
  test_export.cpp)
if(BV_ENABLE_REMOTEPLAY)
  target_sources(blackvolt_engine_tests PRIVATE test_remote_session.cpp)
endif()
target_include_directories(blackvolt_engine_tests PRIVATE ${PROJECT_SOURCE_DIR}/engine/src)
target_link_libraries(blackvolt_engine_tests PRIVATE blackvolt_core)
add_test(NAME blackvolt_engine_tests COMMAND blackvolt_engine_tests)
target_compile_definitions(blackvolt_engine_tests PRIVATE BV_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
