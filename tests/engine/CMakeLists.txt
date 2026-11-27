add_executable(blackvolt_engine_tests
  main.cpp
  test_rolling_stats.cpp
  test_adapter_ranking.cpp
  test_api.cpp
  test_runtime.cpp
  test_player.cpp)
target_include_directories(blackvolt_engine_tests PRIVATE ${PROJECT_SOURCE_DIR}/engine/src)
target_link_libraries(blackvolt_engine_tests PRIVATE blackvolt_core)
add_test(NAME blackvolt_engine_tests COMMAND blackvolt_engine_tests)
target_compile_definitions(blackvolt_engine_tests PRIVATE BV_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
