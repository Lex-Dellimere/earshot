cmake_minimum_required(VERSION 3.24)
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
option(EARSHOT_TESTS  "build the unit tests" OFF)
set(EARSHOT_SANITIZE "" CACHE STRING "sanitizers like address;undefined or thread")

# sanitizers go on everything including whisper so thread checks see all of it
if(EARSHOT_SANITIZE)
    string(REPLACE ";" "," sanitizers "${EARSHOT_SANITIZE}")
    add_compile_options(-fsanitize=${sanitizers} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${sanitizers})
    set(EARSHOT_LTO OFF)
    set(EARSHOT_STRIP OFF)
endif()

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${EARSHOT_LTO})
add_compile_options(-ffunction-sections -fdata-sections $<$<COMPILE_LANGUAGE:C>:-fno-asynchronous-unwind-tables>)
add_link_options(-Wl,--gc-sections -Wl,-O1)

include(FetchContent)

# ggml vulkan needs spirv headers so fetch them when the system has none
if(EARSHOT_VULKAN)
    find_package(SPIRV-Headers CONFIG QUIET)
    if(NOT SPIRV-Headers_FOUND)
        FetchContent_Declare(SPIRV-Headers
            GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
            GIT_TAG vulkan-sdk-1.4.309.0
            GIT_SHALLOW TRUE
            OVERRIDE_FIND_PACKAGE)
        FetchContent_MakeAvailable(SPIRV-Headers)
    endif()
endif()

# whisper cpp is vendored and linked statically
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
if(TARGET ggml-vulkan AND TARGET SPIRV-Headers::SPIRV-Headers)
    target_link_libraries(ggml-vulkan PRIVATE SPIRV-Headers::SPIRV-Headers)
endif()

# everything but main so the unit tests link the same code
add_library(earshot_core OBJECT
    src/config.cpp
    src/ring.cpp
    src/wav.cpp
    src/dsp.cpp
    src/transcribe.cpp
    src/llm.cpp
    src/tui.cpp)
target_compile_options(earshot_core PUBLIC -Wall -Wextra -Wno-missing-field-initializers -fno-exceptions -fno-rtti)
target_link_libraries(earshot_core PUBLIC whisper)

if(WIN32)
    target_sources(earshot_core PRIVATE src/windows/audio_wasapi.cpp src/windows/platform_windows.cpp)
    target_compile_definitions(earshot_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(earshot_core PUBLIC ole32 mmdevapi)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    find_package(Threads REQUIRED)
    target_sources(earshot_core PRIVATE src/linux/audio_pipewire.cpp src/linux/platform_linux.cpp)
    target_link_libraries(earshot_core PUBLIC PkgConfig::PIPEWIRE Threads::Threads ${CMAKE_DL_LIBS})
endif()

add_executable(earshot src/main.cpp)
target_link_libraries(earshot PRIVATE earshot_core)
if(WIN32)
    enable_language(RC)
    target_sources(earshot PRIVATE src/windows/earshot.rc)
    target_link_options(earshot PRIVATE -static)
else()
    target_link_options(earshot PRIVATE -Wl,--as-needed)
endif()

if(EARSHOT_TESTS)
    enable_testing()
    add_executable(earshot-tests tests/test_main.cpp)
    target_include_directories(earshot-tests PRIVATE src)
    target_link_libraries(earshot-tests PRIVATE earshot_core)
    add_test(NAME unit COMMAND earshot-tests)
endif()

if(EARSHOT_STRIP)
    add_custom_command(TARGET earshot POST_BUILD
        COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:earshot>
        COMMENT "stripping earshot")
endif()

install(TARGETS earshot)
