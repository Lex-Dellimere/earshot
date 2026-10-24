cmake_minimum_required(VERSION 3.20)
project(earshot LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(EARSHOT_STRIP  "strip symbols from the binary after linking" ON)
# mingw gcc hits an internal error on ggml simd code under lto or avx512
# so windows builds default to the portable avx2 baseline without lto
if(WIN32)
    option(EARSHOT_LTO "link time optimisation" OFF)
    option(EARSHOT_NATIVE "optimise for the cpu doing the build" OFF)
else()
    option(EARSHOT_LTO "link time optimisation" ON)
    option(EARSHOT_NATIVE "optimise for the cpu doing the build" ON)
endif()
option(EARSHOT_CUDA   "whisper on nvidia gpus (needs nvcc)" OFF)
option(EARSHOT_VULKAN "whisper on any vulkan gpu (needs glslc)" OFF)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${EARSHOT_LTO})
add_compile_options(-ffunction-sections -fdata-sections $<$<COMPILE_LANGUAGE:C>:-fno-asynchronous-unwind-tables>)
add_link_options(-Wl,--gc-sections -Wl,-O1)

# whisper cpp is vendored and linked statically
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE ${EARSHOT_NATIVE} CACHE BOOL "" FORCE)
set(GGML_CUDA ${EARSHOT_CUDA} CACHE BOOL "" FORCE)
set(GGML_VULKAN ${EARSHOT_VULKAN} CACHE BOOL "" FORCE)
FetchContent_Declare(whisper
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG v1.9.5
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(whisper)

add_executable(earshot
    src/main.cpp
    src/config.cpp
    src/ring.cpp
    src/wav.cpp
    src/dsp.cpp
    src/transcribe.cpp
    src/llm.cpp
    src/tui.cpp)
target_compile_options(earshot PRIVATE -Wall -Wextra -Wno-missing-field-initializers -fno-exceptions -fno-rtti)
target_link_libraries(earshot PRIVATE whisper)

if(WIN32)
    enable_language(RC)
    target_sources(earshot PRIVATE src/windows/audio_wasapi.cpp src/windows/platform_windows.cpp src/windows/earshot.rc)
    target_compile_definitions(earshot PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(earshot PRIVATE ole32 mmdevapi)
    target_link_options(earshot PRIVATE -static)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    find_package(Threads REQUIRED)
    target_sources(earshot PRIVATE src/linux/audio_pipewire.cpp src/linux/platform_linux.cpp)
    target_link_libraries(earshot PRIVATE PkgConfig::PIPEWIRE Threads::Threads ${CMAKE_DL_LIBS})
    target_link_options(earshot PRIVATE -Wl,--as-needed)
endif()

if(EARSHOT_STRIP)
    add_custom_command(TARGET earshot POST_BUILD
        COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:earshot>
        COMMENT "stripping earshot")
endif()

install(TARGETS earshot)
