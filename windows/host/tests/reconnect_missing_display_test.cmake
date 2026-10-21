if(NOT DEFINED HOST OR NOT EXISTS "${HOST}")
    message(FATAL_ERROR "wireview_host executable was not provided")
endif()

execute_process(
    COMMAND "${HOST}" --reconnect-desktop 10 --display WIREVIEW_MISSING_DISPLAY
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)

if(result EQUAL 0)
    message(FATAL_ERROR "missing-display reconnect unexpectedly succeeded")
endif()

set(combined "${output}\n${error}")
string(FIND
    "${combined}"
    "requested capture display is unavailable: WIREVIEW_MISSING_DISPLAY"
    expected_error)
if(expected_error EQUAL -1)
    message(FATAL_ERROR "missing-display reconnect returned the wrong error: ${combined}")
endif()

message(STATUS "missing-display reconnect correctly refused primary fallback")
