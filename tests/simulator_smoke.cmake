file(MAKE_DIRECTORY "${TEST_DIR}")
set(expected_chunks 4 1)
set(chunk_sizes 16384 65536)
list(LENGTH chunk_sizes count)
math(EXPR last_index "${count} - 1")

foreach(index RANGE ${last_index})
  list(GET chunk_sizes ${index} chunk_size)
  list(GET expected_chunks ${index} chunks)
  set(output "${TEST_DIR}/fetched-${chunk_size}.bin")
  set(metrics "${TEST_DIR}/metrics-${chunk_size}.json")
  execute_process(
    COMMAND "${PROGRAM}" simulate --input "${SOURCE}" --output "${output}"
            --chunk-size "${chunk_size}" --metrics "${metrics}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "simulator failed: ${stderr}")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${SOURCE}" "${output}"
                  RESULT_VARIABLE compare_result)
  if(NOT compare_result EQUAL 0)
    message(FATAL_ERROR "simulated output differs from input for chunk size ${chunk_size}")
  endif()
  file(READ "${metrics}" report)
  string(FIND "${report}" "\"execution_mode\": \"simulator\"" mode_found)
  string(FIND "${report}" "\"chunks\": ${chunks}" chunks_found)
  string(FIND "${report}" "\"chunk_latency_us\": null" latency_found)
  string(FIND "${report}" "\"integrity_ok\": true" integrity_found)
  if(mode_found EQUAL -1 OR chunks_found EQUAL -1 OR latency_found EQUAL -1 OR integrity_found EQUAL -1)
    message(FATAL_ERROR "simulator metrics labels or expected chunk count are incorrect")
  endif()
endforeach()

file(WRITE "${TEST_DIR}/empty.bin" "")
execute_process(
  COMMAND "${PROGRAM}" simulate --input "${TEST_DIR}/empty.bin"
          --output "${TEST_DIR}/empty-output.bin" --chunk-size 128
  RESULT_VARIABLE empty_result
  ERROR_VARIABLE empty_error
)
if(empty_result EQUAL 0 OR NOT empty_error MATCHES "must not be empty")
  message(FATAL_ERROR "empty input was not rejected with an actionable message")
endif()
