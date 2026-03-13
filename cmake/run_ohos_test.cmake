if(NOT DEFINED HDC_EXECUTABLE OR HDC_EXECUTABLE STREQUAL "")
    message(FATAL_ERROR "HDC_EXECUTABLE is required")
endif()

if(NOT DEFINED DEVICE_DIR OR DEVICE_DIR STREQUAL "")
    message(FATAL_ERROR "DEVICE_DIR is required")
endif()

if(NOT DEFINED TEST_TMPDIR OR TEST_TMPDIR STREQUAL "")
    set(TEST_TMPDIR "${DEVICE_DIR}")
endif()

if(NOT DEFINED HOST_BINARY OR HOST_BINARY STREQUAL "")
    message(FATAL_ERROR "HOST_BINARY is required")
endif()

if(NOT DEFINED REMOTE_BINARY_NAME OR REMOTE_BINARY_NAME STREQUAL "")
    message(FATAL_ERROR "REMOTE_BINARY_NAME is required")
endif()

function(ohos_run_hdc)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE cmd_result
        OUTPUT_VARIABLE cmd_output
        ERROR_VARIABLE cmd_error
    )
    if(NOT cmd_result EQUAL 0)
        message(FATAL_ERROR "Command failed: ${ARGN}\nstdout:\n${cmd_output}\nstderr:\n${cmd_error}")
    endif()
endfunction()

ohos_run_hdc("${HDC_EXECUTABLE}" shell mkdir -p "${DEVICE_DIR}")
ohos_run_hdc("${HDC_EXECUTABLE}" shell mkdir -p "${TEST_TMPDIR}")

set(all_push_files "${HOST_BINARY}")
if(DEFINED PUSH_FILES AND NOT PUSH_FILES STREQUAL "")
    string(REPLACE "|" ";" push_list "${PUSH_FILES}")
    foreach(push_item IN LISTS push_list)
        if(NOT push_item STREQUAL "${HOST_BINARY}")
            list(APPEND all_push_files "${push_item}")
        endif()
    endforeach()
endif()
list(REMOVE_DUPLICATES all_push_files)

foreach(host_file IN LISTS all_push_files)
    get_filename_component(remote_name "${host_file}" NAME)
    ohos_run_hdc("${HDC_EXECUTABLE}" file send "${host_file}" "${DEVICE_DIR}/${remote_name}")
    ohos_run_hdc("${HDC_EXECUTABLE}" shell chmod 755 "${DEVICE_DIR}/${remote_name}")
endforeach()

set(preload_paths)
if(DEFINED PRELOAD_BASENAMES AND NOT PRELOAD_BASENAMES STREQUAL "")
    string(REPLACE "|" ";" preload_names "${PRELOAD_BASENAMES}")
    foreach(preload_name IN LISTS preload_names)
        list(APPEND preload_paths "${DEVICE_DIR}/${preload_name}")
    endforeach()
endif()
string(JOIN ":" preload_value ${preload_paths})

set(arg_list)
if(DEFINED TEST_ARGS AND NOT TEST_ARGS STREQUAL "")
    string(REPLACE "|" ";" arg_list "${TEST_ARGS}")
endif()

set(shell_command "cd ${DEVICE_DIR} && export LD_LIBRARY_PATH=${DEVICE_DIR} && export ALLOC_OVERRIDE_TEST_TMPDIR=${TEST_TMPDIR}")
if(NOT preload_value STREQUAL "")
    string(APPEND shell_command " && export LD_PRELOAD=${preload_value}")
endif()
string(APPEND shell_command " && ./${REMOTE_BINARY_NAME}")
foreach(test_arg IN LISTS arg_list)
    string(APPEND shell_command " '${test_arg}'")
endforeach()

execute_process(
    COMMAND "${HDC_EXECUTABLE}" shell sh -c "${shell_command}"
    RESULT_VARIABLE test_result
    OUTPUT_VARIABLE test_output
    ERROR_VARIABLE test_error
)

if(DEFINED REPORT_REMOTE_PATH AND NOT REPORT_REMOTE_PATH STREQUAL "" AND DEFINED REPORT_LOCAL_PATH AND NOT REPORT_LOCAL_PATH STREQUAL "")
    execute_process(
        COMMAND "${HDC_EXECUTABLE}" file recv "${REPORT_REMOTE_PATH}" "${REPORT_LOCAL_PATH}"
        RESULT_VARIABLE recv_result
        OUTPUT_VARIABLE recv_output
        ERROR_VARIABLE recv_error
    )
    if(NOT recv_result EQUAL 0)
        message(WARNING "Failed to pull report from device\nstdout:\n${recv_output}\nstderr:\n${recv_error}")
    endif()
endif()

if(NOT test_result EQUAL 0)
    message(FATAL_ERROR "OHOS test failed\nstdout:\n${test_output}\nstderr:\n${test_error}")
endif()
