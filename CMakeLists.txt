cmake_minimum_required(VERSION 3.28)
project(blackvolt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BV_BUILD_TESTS "Build engine unit tests" ON)
option(BV_ENABLE_REMOTEPLAY "PlayStation Remote Play (chiaki-ng)" ON)

# Engine binaries land next to the app so the UI finds blackvolt_engine.dll.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_subdirectory(engine)

if(BV_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/engine)
endif()
