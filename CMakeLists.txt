cmake_minimum_required(VERSION 3.20)
project(earshot LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(EARSHOT_CUDA   "Build whisper.cpp with CUDA (needs nvcc)"   OFF)
option(EARSHOT_VULKAN "Build whisper.cpp with Vulkan (needs glslc)" OFF)

# whisper.cpp, vendored and linked statically
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE ON  CACHE BOOL "" FORCE)
set(GGML_CUDA   ${EARSHOT_CUDA}   CACHE BOOL "" FORCE)
set(GGML_VULKAN ${EARSHOT_VULKAN} CACHE BOOL "" FORCE)
FetchContent_Declare(whisper
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG        v1.9.5
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(whisper)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
find_package(Threads REQUIRED)

add_executable(earshot
    src/main.cpp
    src/capture.cpp
    src/ring.cpp
    src/clip.cpp
    src/transcribe.cpp
    src/llm.cpp
    src/tui.cpp)
target_compile_options(earshot PRIVATE -Wall -Wextra -Wno-missing-field-initializers -fno-exceptions -fno-rtti)
target_link_libraries(earshot PRIVATE whisper PkgConfig::PIPEWIRE Threads::Threads)
target_link_options(earshot PRIVATE -s)
install(TARGETS earshot)
