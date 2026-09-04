if(NOT DEFINED EMBED_SCRIPT OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "EMBED_SCRIPT and TEST_DIR are required")
endif()

file(REMOVE_RECURSE "${TEST_DIR}")
file(MAKE_DIRECTORY "${TEST_DIR}")

# Put UTF-8 multi-byte sequences across the 1024-byte source-chunk boundary.
string(REPEAT "a" 1023 zh_prefix)
string(REPEAT "b" 1022 en_prefix)
set(zh "${zh_prefix}你-after-boundary\n第二行 Ω\n")
set(en "${en_prefix}🙂-after-boundary\nsecond line\n")
set(zh_path "${TEST_DIR}/zh.md")
set(en_path "${TEST_DIR}/en.md")
set(header_path "${TEST_DIR}/embedded.h")
file(WRITE "${zh_path}" "${zh}")
file(WRITE "${en_path}" "${en}")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DINPUT_ZH=${zh_path}"
        "-DINPUT_EN=${en_path}"
        "-DOUTPUT=${header_path}"
        -P "${EMBED_SCRIPT}"
    RESULT_VARIABLE embed_result
    OUTPUT_VARIABLE embed_stdout
    ERROR_VARIABLE embed_stderr)
if(NOT embed_result EQUAL 0)
    message(FATAL_ERROR "embed script failed: ${embed_stdout}${embed_stderr}")
endif()

file(READ "${header_path}" generated)
string(FIND "${generated}" "inline constexpr std::string_view zh =" zh_start)
string(FIND "${generated}" "inline constexpr std::string_view en =" en_start)
string(FIND "${generated}" "}  // namespace awj::embedded_changelog" namespace_end)
if(zh_start EQUAL -1 OR en_start EQUAL -1 OR namespace_end EQUAL -1)
    message(FATAL_ERROR "generated header markers are missing")
endif()
math(EXPR zh_length "${en_start} - ${zh_start}")
math(EXPR en_length "${namespace_end} - ${en_start}")
string(SUBSTRING "${generated}" ${zh_start} ${zh_length} zh_block)
string(SUBSTRING "${generated}" ${en_start} ${en_length} en_block)

function(recover_hex output_var block_var)
    string(REGEX MATCHALL "\\\\x[0-9A-Fa-f][0-9A-Fa-f]" escapes "${${block_var}}")
    set(recovered "")
    foreach(escape IN LISTS escapes)
        string(SUBSTRING "${escape}" 2 2 byte_hex)
        string(APPEND recovered "${byte_hex}")
    endforeach()
    string(TOLOWER "${recovered}" recovered)
    set(${output_var} "${recovered}" PARENT_SCOPE)
endfunction()

recover_hex(recovered_zh zh_block)
recover_hex(recovered_en en_block)
string(HEX "${zh}" expected_zh)
string(HEX "${en}" expected_en)
string(TOLOWER "${expected_zh}" expected_zh)
string(TOLOWER "${expected_en}" expected_en)
if(NOT recovered_zh STREQUAL expected_zh)
    message(FATAL_ERROR "zh UTF-8 bytes changed across a chunk boundary")
endif()
if(NOT recovered_en STREQUAL expected_en)
    message(FATAL_ERROR "en UTF-8 bytes changed across a chunk boundary")
endif()

# A 1024-byte chunk expands to exactly 4096 characters of \\xHH escapes.
# Keep every individual C++ literal comfortably below the MSVC C2026 range.
string(REPLACE "\n" ";" generated_lines "${generated}")
foreach(line IN LISTS generated_lines)
    if(line MATCHES "^[ ]+\"\\\\x")
        string(LENGTH "${line}" line_length)
        if(line_length GREATER 4102)
            message(FATAL_ERROR "generated changelog literal is too long: ${line_length}")
        endif()
    endif()
endforeach()
