if(NOT DEFINED INPUT_ZH OR NOT DEFINED INPUT_EN OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "INPUT_ZH, INPUT_EN and OUTPUT are required")
endif()

file(READ "${INPUT_ZH}" changelog_zh)
file(READ "${INPUT_EN}" changelog_en)
string(REPLACE "\r\n" "\n" changelog_zh "${changelog_zh}")
string(REPLACE "\r" "\n" changelog_zh "${changelog_zh}")
string(REPLACE "\r\n" "\n" changelog_en "${changelog_en}")
string(REPLACE "\r" "\n" changelog_en "${changelog_en}")

# Convert normalized UTF-8 bytes to ASCII hex before chunking. CMake's
# string(SUBSTRING) is byte-oriented, so slicing the original text can split a
# multi-byte UTF-8 code point. Hex is ASCII and each input byte is exactly two
# hex characters; slicing only at even offsets cannot split a UTF-8 sequence.
set(AWJ_CHANGELOG_BYTES_PER_LITERAL 1024)
math(EXPR AWJ_CHANGELOG_HEX_PER_LITERAL "${AWJ_CHANGELOG_BYTES_PER_LITERAL} * 2")

function(awj_make_hex_literals output_var input_var)
    string(HEX "${${input_var}}" content_hex)
    string(LENGTH "${content_hex}" hex_length)
    set(generated "")
    set(offset 0)
    while(offset LESS hex_length)
        math(EXPR remaining "${hex_length} - ${offset}")
        if(remaining GREATER AWJ_CHANGELOG_HEX_PER_LITERAL)
            set(chunk_length ${AWJ_CHANGELOG_HEX_PER_LITERAL})
        else()
            set(chunk_length ${remaining})
        endif()
        math(EXPR chunk_remainder "${chunk_length} % 2")
        if(NOT chunk_remainder EQUAL 0)
            message(FATAL_ERROR "internal changelog hex chunk is not byte-aligned")
        endif()
        string(SUBSTRING "${content_hex}" ${offset} ${chunk_length} chunk_hex)
        set(escaped "")
        set(byte_offset 0)
        while(byte_offset LESS chunk_length)
            string(SUBSTRING "${chunk_hex}" ${byte_offset} 2 byte_hex)
            string(APPEND escaped "\\x${byte_hex}")
            math(EXPR byte_offset "${byte_offset} + 2")
        endwhile()
        string(APPEND generated "    \"${escaped}\"\n")
        math(EXPR offset "${offset} + ${chunk_length}")
    endwhile()
    if(hex_length EQUAL 0)
        set(generated "    \"\"\n")
    endif()
    set(${output_var} "${generated}" PARENT_SCOPE)
endfunction()

awj_make_hex_literals(changelog_zh_literals changelog_zh)
awj_make_hex_literals(changelog_en_literals changelog_en)

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
file(WRITE "${OUTPUT}" "#pragma once\n"
    "#include <string_view>\n\n"
    "namespace awj::embedded_changelog {\n"
    "inline constexpr std::string_view zh =\n${changelog_zh_literals}    ;\n"
    "inline constexpr std::string_view en =\n${changelog_en_literals}    ;\n"
    "}  // namespace awj::embedded_changelog\n")
