if (NOT DEFINED OUTPUT OR
    NOT DEFINED VERTEX_SPV OR
    NOT DEFINED FRAGMENT_SPV OR
    NOT DEFINED NAMESPACE)
    message(FATAL_ERROR
        "OUTPUT, VERTEX_SPV, FRAGMENT_SPV, and NAMESPACE are required")
endif()

function(generate_spirv_array input_file symbol output_variable)
    if (NOT EXISTS "${input_file}")
        message(FATAL_ERROR "SPIR-V input file does not exist: ${input_file}")
    endif()

    file(SIZE "${input_file}" byte_count)
    math(EXPR byte_remainder "${byte_count} % 4")
    if (byte_count EQUAL 0 OR NOT byte_remainder EQUAL 0)
        message(FATAL_ERROR
            "SPIR-V input must be non-empty and four-byte aligned: ${input_file}")
    endif()

    file(READ "${input_file}" hex_data HEX)
    math(EXPR element_count "${byte_count} / 4")
    math(EXPR last_element_index "${element_count} - 1")

    set(array_text
        "    inline constexpr std::array<uint32_t, ${element_count}> ${symbol}{\n")
    set(values_on_line 0)

    foreach(element_index RANGE 0 ${last_element_index})
        math(EXPR byte_offset "${element_index} * 8")
        math(EXPR byte_1_offset "${byte_offset} + 2")
        math(EXPR byte_2_offset "${byte_offset} + 4")
        math(EXPR byte_3_offset "${byte_offset} + 6")

        string(SUBSTRING "${hex_data}" ${byte_offset} 2 byte_0)
        string(SUBSTRING "${hex_data}" ${byte_1_offset} 2 byte_1)
        string(SUBSTRING "${hex_data}" ${byte_2_offset} 2 byte_2)
        string(SUBSTRING "${hex_data}" ${byte_3_offset} 2 byte_3)
        set(word "0x${byte_3}${byte_2}${byte_1}${byte_0}")

        string(APPEND array_text "        ${word}")
        math(EXPR values_on_line "${values_on_line} + 1")
        if (element_index LESS last_element_index)
            string(APPEND array_text ",")
        endif()

        if (values_on_line EQUAL 6 OR element_index EQUAL last_element_index)
            string(APPEND array_text "\n")
            set(values_on_line 0)
        else()
            string(APPEND array_text " ")
        endif()
    endforeach()

    string(APPEND array_text "    };")
    set(${output_variable} "${array_text}" PARENT_SCOPE)
endfunction()

get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")

generate_spirv_array("${VERTEX_SPV}" kVertex vertex_array)
generate_spirv_array("${FRAGMENT_SPV}" kFragment fragment_array)

set(generated_content "#pragma once\n\n")
string(APPEND generated_content "#include <array>\n")
string(APPEND generated_content "#include <cstdint>\n\n")
string(APPEND generated_content "namespace ${NAMESPACE} {\n")
string(APPEND generated_content "${vertex_array}\n\n")
string(APPEND generated_content "${fragment_array}\n")
string(APPEND generated_content "} // namespace ${NAMESPACE}\n")

set(should_write TRUE)
if (EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" existing_content)
    if (existing_content STREQUAL generated_content)
        set(should_write FALSE)
    endif()
endif()

if (should_write)
    file(WRITE "${OUTPUT}" "${generated_content}")
endif()
