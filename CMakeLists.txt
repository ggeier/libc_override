cmake_minimum_required(VERSION 3.21)

project(alloc_return_override C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

enable_testing()

find_package(Threads QUIET)

option(ALLOC_OVERRIDE_ENABLE_STRICT_NEGATIVE_TESTS "Enable strict mock-only negative tests" OFF)
option(ALLOC_OVERRIDE_RUN_DEVICE_TESTS "Run tests on an OHOS device via hdc" OFF)

set(ALLOC_OVERRIDE_DEFAULT_HDC
    "${CMAKE_CURRENT_SOURCE_DIR}/ohos-sdk/linux/toolchains-linux-x64-6.0.0.48-Release/toolchains/hdc"
)
if(NOT DEFINED HDC_EXECUTABLE AND EXISTS "${ALLOC_OVERRIDE_DEFAULT_HDC}")
    set(HDC_EXECUTABLE "${ALLOC_OVERRIDE_DEFAULT_HDC}" CACHE FILEPATH "Path to the hdc executable")
endif()

set(ALLOC_OVERRIDE_DEVICE_DIR "/data/local/tmp/alloc_override_tests" CACHE STRING "Target directory on the OHOS device for test artifacts")
set(ALLOC_OVERRIDE_TEST_TMPDIR "${ALLOC_OVERRIDE_DEVICE_DIR}" CACHE STRING "Temporary directory used by tests on the target")

function(alloc_override_link_platform_libs target_name)
    if(CMAKE_DL_LIBS)
        target_link_libraries(${target_name} PRIVATE ${CMAKE_DL_LIBS})
    endif()
    if(TARGET Threads::Threads)
        target_link_libraries(${target_name} PRIVATE Threads::Threads)
    endif()
endfunction()

function(alloc_override_add_host_test test_name)
    add_test(NAME ${test_name} ${ARGN})
endfunction()

function(alloc_override_add_device_test test_name test_target)
    set(options)
    set(one_value_args report_local report_remote)
    set(multi_value_args preload_targets test_args)
    cmake_parse_arguments(AODT "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    set(push_paths "$<TARGET_FILE:${test_target}>")
    set(preload_basenames)

    foreach(preload_target IN LISTS AODT_preload_targets)
        list(APPEND push_paths "$<TARGET_FILE:${preload_target}>")
        list(APPEND preload_basenames "$<TARGET_FILE_NAME:${preload_target}>")
    endforeach()

    string(JOIN "|" push_paths_joined ${push_paths})
    string(JOIN "|" preload_joined ${preload_basenames})
    string(JOIN "|" test_args_joined ${AODT_test_args})

    add_test(
        NAME ${test_name}
        COMMAND
            ${CMAKE_COMMAND}
            -DHDC_EXECUTABLE=${HDC_EXECUTABLE}
            -DDEVICE_DIR=${ALLOC_OVERRIDE_DEVICE_DIR}
            -DTEST_TMPDIR=${ALLOC_OVERRIDE_TEST_TMPDIR}
            -DHOST_BINARY=$<TARGET_FILE:${test_target}>
            -DREMOTE_BINARY_NAME=$<TARGET_FILE_NAME:${test_target}>
            -DPUSH_FILES=${push_paths_joined}
            -DPRELOAD_BASENAMES=${preload_joined}
            -DTEST_ARGS=${test_args_joined}
            -DREPORT_LOCAL_PATH=${AODT_report_local}
            -DREPORT_REMOTE_PATH=${AODT_report_remote}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_ohos_test.cmake
    )
endfunction()

add_library(alloc_return_override SHARED
    src/dirent_overrides.c
    src/helpers.c
    src/path_overrides.c
    src/stdio_overrides.c
    src/string_overrides.c
)

target_compile_definitions(alloc_return_override PRIVATE
    _GNU_SOURCE
    _LARGEFILE64_SOURCE
)

target_compile_options(alloc_return_override PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    target_link_options(alloc_return_override PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/alloc_return_override_ohos.map"
    )
else()
    target_link_options(alloc_return_override PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/alloc_return_override.map"
    )
endif()

add_library(mock_allocator SHARED
    src/mock_allocator.c
)

target_compile_definitions(mock_allocator PRIVATE
    _GNU_SOURCE
)

target_include_directories(mock_allocator PUBLIC
    include
)

target_compile_options(mock_allocator PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

alloc_override_link_platform_libs(mock_allocator)

if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    target_link_options(mock_allocator PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/mock_allocator_ohos.map"
    )
else()
    target_link_options(mock_allocator PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/mock_allocator.map"
    )
endif()

add_executable(phase1a_smoke tests/phase1a_smoke.c)

add_executable(phase1a_validation tests/phase1a_validation.c)

add_executable(phase1a_mock_only tests/phase1a_mock_only.c)

target_compile_definitions(phase1a_smoke PRIVATE
    _GNU_SOURCE
    _LARGEFILE64_SOURCE
)

target_compile_definitions(phase1a_validation PRIVATE
    _GNU_SOURCE
    _LARGEFILE64_SOURCE
)

target_compile_definitions(phase1a_mock_only PRIVATE
    _GNU_SOURCE
    _LARGEFILE64_SOURCE
)

alloc_override_link_platform_libs(phase1a_smoke)
alloc_override_link_platform_libs(phase1a_validation)
alloc_override_link_platform_libs(phase1a_mock_only)

target_include_directories(phase1a_validation PRIVATE
    include
)

target_include_directories(phase1a_mock_only PRIVATE
    include
)

target_compile_options(phase1a_smoke PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_compile_options(phase1a_validation PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_compile_options(phase1a_mock_only PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    if(ALLOC_OVERRIDE_RUN_DEVICE_TESTS)
        if(NOT HDC_EXECUTABLE)
            message(FATAL_ERROR "ALLOC_OVERRIDE_RUN_DEVICE_TESTS requires HDC_EXECUTABLE")
        endif()

        alloc_override_add_device_test(
            phase1a_resolution_smoke
            phase1a_smoke
            preload_targets alloc_return_override mock_allocator
            test_args $<TARGET_FILE_NAME:alloc_return_override>
        )

        alloc_override_add_device_test(
            phase1a_allocator_validation
            phase1a_validation
            preload_targets alloc_return_override mock_allocator
            test_args $<TARGET_FILE_NAME:mock_allocator>
        )

        alloc_override_add_device_test(
            phase1a_mock_only_diagnostic
            phase1a_mock_only
            preload_targets mock_allocator
            test_args
                $<TARGET_FILE_NAME:mock_allocator>
                ${ALLOC_OVERRIDE_DEVICE_DIR}/phase1a_mock_only_diagnostic.txt
                diagnostic
            report_local ${CMAKE_CURRENT_BINARY_DIR}/phase1a_mock_only_diagnostic.txt
            report_remote ${ALLOC_OVERRIDE_DEVICE_DIR}/phase1a_mock_only_diagnostic.txt
        )

        if(ALLOC_OVERRIDE_ENABLE_STRICT_NEGATIVE_TESTS)
            alloc_override_add_device_test(
                phase1a_mock_only_strict_negative
                phase1a_mock_only
                preload_targets mock_allocator
                test_args
                    $<TARGET_FILE_NAME:mock_allocator>
                    ${ALLOC_OVERRIDE_DEVICE_DIR}/phase1a_mock_only_strict.txt
                    strict_negative
                report_local ${CMAKE_CURRENT_BINARY_DIR}/phase1a_mock_only_strict.txt
                report_remote ${ALLOC_OVERRIDE_DEVICE_DIR}/phase1a_mock_only_strict.txt
            )
        endif()
    else()
        message(STATUS "OHOS cross build configured without device tests. Set ALLOC_OVERRIDE_RUN_DEVICE_TESTS=ON to register CTest device runs.")
    endif()
else()
    alloc_override_add_host_test(
        phase1a_resolution_smoke
        COMMAND
            ${CMAKE_COMMAND} -E env
            "LD_PRELOAD=$<TARGET_FILE:alloc_return_override>:$<TARGET_FILE:mock_allocator>"
            $<TARGET_FILE:phase1a_smoke>
            $<TARGET_FILE_NAME:alloc_return_override>
    )

    alloc_override_add_host_test(
        phase1a_allocator_validation
        COMMAND
            ${CMAKE_COMMAND} -E env
            "LD_PRELOAD=$<TARGET_FILE:alloc_return_override>:$<TARGET_FILE:mock_allocator>"
            $<TARGET_FILE:phase1a_validation>
            $<TARGET_FILE_NAME:mock_allocator>
    )

    alloc_override_add_host_test(
        phase1a_mock_only_diagnostic
        COMMAND
            ${CMAKE_COMMAND} -E env
            "LD_PRELOAD=$<TARGET_FILE:mock_allocator>"
            $<TARGET_FILE:phase1a_mock_only>
            $<TARGET_FILE_NAME:mock_allocator>
            "${CMAKE_CURRENT_BINARY_DIR}/phase1a_mock_only_diagnostic.txt"
            diagnostic
    )

    if(ALLOC_OVERRIDE_ENABLE_STRICT_NEGATIVE_TESTS)
        alloc_override_add_host_test(
            phase1a_mock_only_strict_negative
            COMMAND
                ${CMAKE_COMMAND} -E env
                "LD_PRELOAD=$<TARGET_FILE:mock_allocator>"
                $<TARGET_FILE:phase1a_mock_only>
                $<TARGET_FILE_NAME:mock_allocator>
                "${CMAKE_CURRENT_BINARY_DIR}/phase1a_mock_only_strict.txt"
                strict_negative
        )
    endif()
endif()
