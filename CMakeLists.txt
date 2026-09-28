cmake_minimum_required(VERSION 3.24)
project(SmoothMotionHost VERSION 0.4.0 LANGUAGES CXX)

option(SMH_CORE_TESTS_ONLY "Build only the portable decision/argument tests" OFF)
include(CTest)

if(NOT SMH_CORE_TESTS_ONLY)
    if(NOT WIN32 OR NOT MSVC)
        message(FATAL_ERROR "Build the host with MSVC on Windows (see the included GitHub Actions workflow). For portable tests use -DSMH_CORE_TESTS_ONLY=ON.")
    endif()
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "SmoothMotionHost requires x64. Configure with -A x64.")
    endif()
    add_executable(SmoothMotionHost host.cpp)
    target_compile_features(SmoothMotionHost PRIVATE cxx_std_20)
    target_compile_definitions(SmoothMotionHost PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    target_compile_options(SmoothMotionHost PRIVATE /W4 /EHsc /permissive- /Zc:__cplusplus /utf-8)
    # Static CRT: the Actions artifact does not need a separate VC runtime installer.
    set_property(TARGET SmoothMotionHost PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_libraries(SmoothMotionHost PRIVATE d3d11 dxgi d3dcompiler windowsapp user32 dwmapi)
    if(BUILD_TESTING)
        # These run without a GPU, capture support, an interactive desktop or SM86.
        add_test(NAME cli_help COMMAND SmoothMotionHost --help)
        add_test(NAME cli_version COMMAND SmoothMotionHost --version)
        set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "0.4.0")
        add_test(NAME cli_bad_option COMMAND SmoothMotionHost --not-a-real-option)
        set_tests_properties(cli_bad_option PROPERTIES WILL_FAIL TRUE)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(SmoothMotionHostCoreTests core_tests.cpp)
    target_include_directories(SmoothMotionHostCoreTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(SmoothMotionHostCoreTests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(SmoothMotionHostCoreTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    else()
        target_compile_options(SmoothMotionHostCoreTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME core_regressions COMMAND SmoothMotionHostCoreTests)
endif()
