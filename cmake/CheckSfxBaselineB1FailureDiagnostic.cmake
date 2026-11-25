if(NOT DEFINED SFX_BASELINE_B1_EXECUTABLE OR
   "${SFX_BASELINE_B1_EXECUTABLE}" STREQUAL "")
    message(FATAL_ERROR "SFX_BASELINE_B1_EXECUTABLE is required")
endif()

execute_process(
    COMMAND "${SFX_BASELINE_B1_EXECUTABLE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE standard_output
    ERROR_VARIABLE standard_error
)
if(result EQUAL 0)
    message(FATAL_ERROR "argument-validation failure invocation unexpectedly passed")
endif()
string(FIND "${standard_output}"
    "SFX Baseline Window B1 test failure during argument validation after 0 assertions: expected one scratch-directory argument"
    diagnostic_position)
if(diagnostic_position EQUAL -1)
    message(FATAL_ERROR
        "visible standard-output failure diagnostic missing\n"
        "stdout: ${standard_output}\n"
        "stderr: ${standard_error}")
endif()

message(STATUS "SFX Baseline Window B1 visible failure diagnostic test passed")
