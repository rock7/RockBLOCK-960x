# --- at-client (GoogleTest) --------------------------------------------------

enable_language(CXX)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)

    # DOWNLOAD_EXTRACT_TIMESTAMP itself needs CMake 3.24+; older FetchContent
    # (and the ExternalProject_Add it forwards to) doesn't recognise the
    # keyword at all, so only pass it when the running CMake supports it.
    set(_rb960x_gtest_extra_args "")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24")
        list(APPEND _rb960x_gtest_extra_args DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    endif()

    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        ${_rb960x_gtest_extra_args})
    # Keep gtest's CRT choice consistent on Windows builds.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# The C sources are built again for the tests with RB960X_TEST_CLOCK
# defined, so DELAY_MS and MILLIS run on the mock port's clock
# (test_time.cpp) instead of real time; see src/util.h. A define is used
# rather than force-including test_time.h, as that didn't reach the C
# sources in Visual Studio builds. initRat sets RB960X_INIT_RAT for that
# copy.
add_library(rb960x_testtime STATIC test_time.cpp)
target_include_directories(rb960x_testtime PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

function(rb960x_add_testlib name initRat)
    add_library(${name} STATIC
        ${PROJECT_SOURCE_DIR}/src/at_client.c
        ${PROJECT_SOURCE_DIR}/src/rockblock960x.c)
    target_include_directories(${name} PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${name} PUBLIC rb960x_util rb960x_crossplatform rb960x_testtime)
    target_compile_definitions(${name} PUBLIC RB960X_INIT_RAT=${initRat})
    # src/util.h includes test_time.h when this is set, so the driver
    # runs on the mock port's clock.
    target_compile_definitions(${name} PRIVATE RB960X_TEST_CLOCK=1)

    # rb960xBegin()/rb960xEnd() only exist where a serial port is built in.
    if(TARGET atclient_port)
        target_sources(${name} PRIVATE
            ${PROJECT_SOURCE_DIR}/src/rockblock960x_begin.c
            ${PROJECT_SOURCE_DIR}/${_rb960x_port_sources})
    endif()
endfunction()

# Most tests script the 9604's starting RAT themselves, so init leaves it.
rb960x_add_testlib(rb960x_testlib RB960X_INIT_RAT_NONE)

add_executable(atclient_tests test_at_client.cpp)

target_link_libraries(atclient_tests PRIVATE rb960x_testlib GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(atclient_tests)

# --- rockblock960x (GoogleTest) ----------------------------------------------

add_executable(rockblock960x_tests test_rockblock960x.cpp)

target_link_libraries(rockblock960x_tests PRIVATE rb960x_testlib GTest::gtest_main)

# The real-port tests drive the POSIX port through a pseudo-terminal.
if(TARGET atclient_port AND NOT WIN32)
    target_compile_definitions(rockblock960x_tests PRIVATE RB960X_TEST_HAVE_PORT=1)
endif()

gtest_discover_tests(rockblock960x_tests)

# --- RB960X_INIT_RAT (GoogleTest) --------------------------------------------
# test_init_rat.cpp is built once per setting, against a driver built with
# that setting.

foreach(_rat SBD CELLULAR)
    rb960x_add_testlib(rb960x_testlib_rat_${_rat} RB960X_INIT_RAT_${_rat})
    add_executable(rockblock960x_init_rat_${_rat}_tests test_init_rat.cpp)
    target_link_libraries(rockblock960x_init_rat_${_rat}_tests PRIVATE rb960x_testlib_rat_${_rat} GTest::gtest_main)
    gtest_discover_tests(rockblock960x_init_rat_${_rat}_tests TEST_PREFIX "InitRat${_rat}.")
endforeach()
