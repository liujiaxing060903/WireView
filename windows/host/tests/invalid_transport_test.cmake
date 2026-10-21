if(NOT DEFINED HOST OR NOT EXISTS "${HOST}")
    message(FATAL_ERROR "wireview_host executable was not provided")
endif()

execute_process(
    COMMAND "${HOST}" --stream-desktop 1 --transport invalid
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)

if(result EQUAL 0)
    message(FATAL_ERROR "invalid desktop transport unexpectedly succeeded")
endif()

set(combined "${output}\n${error}")
string(FIND
    "${combined}"
    "desktop stream transport must be adb or winusb"
    expected_error)
if(expected_error EQUAL -1)
    message(FATAL_ERROR "invalid desktop transport returned the wrong error: ${combined}")
endif()

message(STATUS "invalid desktop transport was rejected before opening a device")
