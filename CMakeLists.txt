# RockBLOCK-960x — desktop build (Linux, macOS and Windows) and the shared library
# consumed by the Python bindings. Arduino/PlatformIO never see this file;
# they compile src/ directly via library.properties / library.json.
#
# Target usage:
#   cmake -B build && cmake --build build && ctest --test-dir build
#   cmake -B build -DBUILD_SHARED_LIBS=ON     # .so/.dylib/.dll for Python

cmake_minimum_required(VERSION 3.14) # FetchContent_MakeAvailable() in tests/
project(rockblock-960x)

# PROJECT_IS_TOP_LEVEL needs CMake 3.21; this is the pre-3.21 equivalent
# (true when this is the CMakeLists.txt CMake was actually invoked on, not
# one pulled in via add_subdirectory()/FetchContent by an embedding project).
set(RB960X_IS_TOP_LEVEL FALSE)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(RB960X_IS_TOP_LEVEL TRUE)
endif()

# Version comes from the header (single source of truth) never hardcode
# it here. See scripts/version.py.
file(STRINGS src/rockblock960x.h _rb960x_version_line
     REGEX "#define RB960X_VERSION_STRING")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" ROCKBLOCK960X_VERSION
       "${_rb960x_version_line}")
if(NOT ROCKBLOCK960X_VERSION)
    message(FATAL_ERROR "Could not parse RB960X_VERSION_STRING from src/rockblock960x.h")
endif()

project(rockblock960x VERSION ${ROCKBLOCK960X_VERSION} LANGUAGES C)

# Visual Studio (MSVC): its default C mode predates parts of C99 the code
# uses, so build as C11 (/std:c11, Visual Studio 2019 16.8 or later), and
# don't warn about standard functions such as strncpy() and snprintf().
if(MSVC)
    if(NOT DEFINED CMAKE_C_STANDARD)
        set(CMAKE_C_STANDARD 11)
    endif()
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# Internal Util Helper
add_library(rb960x_util src/util.c)
target_include_directories(rb960x_util PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_compile_features(rb960x_util PUBLIC c_std_99)

# Internal CrossPlatform Helper
add_library(rb960x_crossplatform src/crossplatform.c)
target_include_directories(rb960x_crossplatform PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_compile_features(rb960x_crossplatform PUBLIC c_std_99)

# --- Core library (rockblock960x) --------------------------------------------
# List sources explicitly (globs hide typos and confuse regeneration).

add_library(rockblock960x src/rockblock960x.c)

target_include_directories(rockblock960x PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>)
target_compile_features(rockblock960x PUBLIC c_std_99)
target_link_libraries(rockblock960x PUBLIC rb960x_util rb960x_crossplatform)

add_library(atclient src/at_client.c)

target_include_directories(atclient PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>)
target_compile_features(atclient PUBLIC c_std_99)
target_link_libraries(atclient PUBLIC rb960x_util)

# rockblock960x builds its AT command handling on top of atclient (see
# src/rockblock960x.c); linked here rather than up next to
# add_library(rockblock960x ...) because atclient must already exist as a
# target for target_link_libraries() to treat it as one.
target_link_libraries(rockblock960x PUBLIC atclient)

# --- AT port (atclient_port) -------------------------------------------------
# One target for the serial port behind src/at_port.h. CMake compiles the
# implementation for the platform being built, so applications just link
# atclient_port and include at_port.h, whatever the target.
# To add a platform: write ports/<name>/at_port_<name>.c and add a branch.
#
# Where there is a port, rb960xBegin()/rb960xEnd() (src/rockblock960x_begin.c)
# are built into rockblock960x on top of it, so applications only need to
# link rockblock960x. Without one, use rb960xInit() with your own callbacks.

if((CMAKE_SYSTEM_NAME STREQUAL "Linux") OR APPLE)
    set(_rb960x_port_sources ports/posix/at_port_posix.c)
elseif(WIN32)
    set(_rb960x_port_sources ports/windows/at_port_windows.c)
endif()

if(_rb960x_port_sources)
    add_library(atclient_port ${_rb960x_port_sources})
    target_link_libraries(atclient_port PUBLIC atclient)

    target_sources(rockblock960x PRIVATE src/rockblock960x_begin.c)
    target_link_libraries(rockblock960x PUBLIC atclient_port)
else()
    message(STATUS "rockblock960x: no AT port for ${CMAKE_SYSTEM_NAME} yet; "
                   "atclient_port and the examples are not built")
endif()

# --- Examples — only when built standalone, mirrors the tests gate below ----

if(TARGET atclient_port AND RB960X_IS_TOP_LEVEL)
    # The examples parse options with getopt_long(). Visual Studio has no
    # getopt.h, so build wingetopt (third_party/wingetopt, ISC and BSD
    # licences) for them there. Every other toolchain has its own.
    if(MSVC)
        add_library(wingetopt STATIC third_party/wingetopt/src/getopt.c)
        target_include_directories(wingetopt PUBLIC third_party/wingetopt/src)
    endif()

    # Cross-platform: only rb960xBegin() and crossplatform.h, so these
    # build wherever atclient_port exists.
    add_executable(rb960x_device_info examples/device_info.c)
    target_link_libraries(rb960x_device_info PRIVATE rockblock960x)
    add_executable(rb960x_sdb_send examples/sdb_send.c)
    target_link_libraries(rb960x_sdb_send PRIVATE rockblock960x)
    add_executable(rb960x_sdb_send_async examples/sdb_send_async.c)
    target_link_libraries(rb960x_sdb_send_async PRIVATE rockblock960x)
    add_executable(rb960x_sdb_receive examples/sdb_receive.c)
    target_link_libraries(rb960x_sdb_receive PRIVATE rockblock960x)
    add_executable(rb960x_send_receive examples/send_receive.c)
    target_link_libraries(rb960x_send_receive PRIVATE rockblock960x)

    if(MSVC)
        foreach(_rb960x_example rb960x_device_info rb960x_sdb_send rb960x_sdb_send_async rb960x_sdb_receive)
            target_link_libraries(${_rb960x_example} PRIVATE wingetopt)
        endforeach()
    endif()
endif()

# --- Python bindings (rockblock960x_python) -----------------------------------
# One shared library holding the driver plus the helpers in
# python/rb960x_python.c, loaded by python/rockblock960x with ctypes. The
# serial port is done in Python (pyserial), so
# it doesn't need atclient_port and builds on any desktop platform.
option(RB960X_BUILD_PYTHON "Build the shared library for the Python bindings"
       ${RB960X_IS_TOP_LEVEL})

if(RB960X_BUILD_PYTHON)
    # Compile the driver sources straight in (rather than linking the static
    # libraries) so every driver function is exported, on every platform,
    # without whole-archive or -fPIC tricks.
    add_library(rockblock960x_python SHARED
        python/rb960x_python.c
        src/rockblock960x.c
        src/at_client.c
        src/util.c
        src/crossplatform.c)
    target_include_directories(rockblock960x_python PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(rockblock960x_python PRIVATE c_std_99)
    if(WIN32)
        set_target_properties(rockblock960x_python PROPERTIES
            WINDOWS_EXPORT_ALL_SYMBOLS ON)
    endif()

    # pip install (scikit-build-core sets SKBUILD): put the library inside
    # the Python package, where python/rockblock960x/_native.py looks first.
    if(SKBUILD)
        install(TARGETS rockblock960x_python
            LIBRARY DESTINATION rockblock960x COMPONENT python
            RUNTIME DESTINATION rockblock960x COMPONENT python)
    endif()
endif()

# --- Warnings — gate on the compiler ------------------------------------------
foreach(_rb960x_target rockblock960x atclient rb960x_util rb960x_crossplatform)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${_rb960x_target} PRIVATE -Wall -Wextra -Wconversion)
    elseif(MSVC)
        target_compile_options(${_rb960x_target} PRIVATE /W4)
    endif()
endforeach()

foreach(_rb960x_target atclient_port rb960x_device_info rb960x_sdb_send rb960x_sdb_send_async rb960x_sdb_receive rb960x_send_receive rockblock960x_python)
    if(TARGET ${_rb960x_target})
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${_rb960x_target} PRIVATE -Wall -Wextra -Wconversion)
        elseif(MSVC)
            target_compile_options(${_rb960x_target} PRIVATE /W4)
        endif()
    endif()
endforeach()

# --- Documentation (docs) ----------------------------------------------------
# `cmake --build build --target docs` regenerates the Doxygen HTML into docs/
# at the repository root, which is committed and published on GitHub Pages.
# Only offered when built standalone, like the examples and tests.
if(RB960X_IS_TOP_LEVEL)
    find_package(Doxygen)
    if(DOXYGEN_FOUND)
        add_custom_target(docs
            COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/rockblock-960x-doxygen.config
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Generating API documentation with Doxygen"
            VERBATIM)
    else()
        message(STATUS "Doxygen not found; the 'docs' target will not be available")
    endif()
endif()

# --- Tests — but only when built standalone, so embedding projects don't
#     inherit them --------------------------------------------------------
include(CTest)
if(BUILD_TESTING AND RB960X_IS_TOP_LEVEL)
    add_subdirectory(tests)
endif()
