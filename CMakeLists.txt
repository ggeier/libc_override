cmake_minimum_required(VERSION 3.21)

project(alloc_return_override C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

enable_testing()

find_package(Threads REQUIRED)

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

target_link_options(alloc_return_override PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/alloc_return_override.map"
)

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

target_link_libraries(mock_allocator PRIVATE
    Threads::Threads
    dl
)

target_link_options(mock_allocator PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/mock_allocator.map"
)

add_executable(phase1a_smoke tests/phase1a_smoke.c)

add_executable(phase1a_validation tests/phase1a_validation.c)

target_compile_definitions(phase1a_smoke PRIVATE
    _GNU_SOURCE
    _LARGEFILE64_SOURCE
)

target_compile_definitions(phase1a_validation PRIVATE
    _GNU_SOURCE
    _LARGEFILE64_SOURCE
)

target_link_libraries(phase1a_smoke PRIVATE dl)

target_link_libraries(phase1a_validation PRIVATE dl)

target_include_directories(phase1a_validation PRIVATE
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

add_test(
    NAME phase1a_resolution_smoke
    COMMAND
        ${CMAKE_COMMAND} -E env
        "LD_PRELOAD=$<TARGET_FILE:alloc_return_override>:$<TARGET_FILE:mock_allocator>"
        $<TARGET_FILE:phase1a_smoke>
        $<TARGET_FILE_NAME:alloc_return_override>
)

add_test(
    NAME phase1a_allocator_validation
    COMMAND
        ${CMAKE_COMMAND} -E env
        "LD_PRELOAD=$<TARGET_FILE:alloc_return_override>:$<TARGET_FILE:mock_allocator>"
        $<TARGET_FILE:phase1a_validation>
        $<TARGET_FILE_NAME:mock_allocator>
)
