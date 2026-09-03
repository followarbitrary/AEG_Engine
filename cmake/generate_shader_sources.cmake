if (NOT DEFINED OUTPUT_HEADER OR
    NOT DEFINED OUTPUT_SOURCE OR
    NOT DEFINED NAMESPACE OR
    NOT DEFINED SYMBOL_PREFIX OR
    NOT DEFINED SHADER_SPECS)
    message(FATAL_ERROR
        "OUTPUT_HEADER, OUTPUT_SOURCE, NAMESPACE, SYMBOL_PREFIX, and SHADER_SPECS are required")
endif()

if (NOT SYMBOL_PREFIX MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
    message(FATAL_ERROR
        "Shader symbol prefix is not a valid identifier: ${SYMBOL_PREFIX}")
endif()

function(write_if_different output_file content)
    set(should_write TRUE)
    if (EXISTS "${output_file}")
        file(READ "${output_file}" existing_content)
        if (existing_content STREQUAL content)
            set(should_write FALSE)
        endif()
    endif()

    if (should_write)
        file(WRITE "${output_file}" "${content}")
    endif()
endfunction()

function(generate_spirv_array input_file data_symbol output_variable)
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
        "        alignas(4) constexpr std::array<std::uint32_t, ${element_count}> ${data_symbol}{\n")
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

        string(APPEND array_text "            ${word}")
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

    string(APPEND array_text "        };")
    set(${output_variable} "${array_text}" PARENT_SCOPE)
endfunction()

get_filename_component(header_directory "${OUTPUT_HEADER}" DIRECTORY)
get_filename_component(source_directory "${OUTPUT_SOURCE}" DIRECTORY)
get_filename_component(header_name "${OUTPUT_HEADER}" NAME)
file(MAKE_DIRECTORY "${header_directory}")
file(MAKE_DIRECTORY "${source_directory}")

set(header_content "#pragma once\n\n")
string(APPEND header_content "#include \"shader_binary.hpp\"\n\n")
string(APPEND header_content "namespace ${NAMESPACE} {\n")

set(source_content "#include \"${header_name}\"\n\n")
string(APPEND source_content "#include <array>\n")
string(APPEND source_content "#include <cstdint>\n\n")
string(APPEND source_content "namespace ${NAMESPACE} {\n")
string(APPEND source_content "    namespace {\n")

set(shader_symbols)
foreach(shader_spec IN LISTS SHADER_SPECS)
    string(REPLACE "|" ";" shader_fields "${shader_spec}")
    list(LENGTH shader_fields shader_field_count)
    if (shader_field_count LESS 3 OR shader_field_count GREATER 4)
        message(FATAL_ERROR "Invalid shader specification: ${shader_spec}")
    endif()

    list(GET shader_fields 0 stage_name)
    list(GET shader_fields 1 spv_file)
    if (NOT stage_name MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "Shader stage name is not a valid identifier: ${stage_name}")
    endif()
    string(REPLACE "_" ";" stage_parts "${stage_name}")
    set(stage_identifier)
    foreach(stage_part IN LISTS stage_parts)
        if (NOT stage_part)
            continue()
        endif()
        string(SUBSTRING "${stage_part}" 0 1 first_character)
        string(TOUPPER "${first_character}" first_character_upper)
        string(SUBSTRING "${stage_part}" 1 -1 remaining_characters)
        string(APPEND stage_identifier
            "${first_character_upper}${remaining_characters}")
    endforeach()
    set(symbol "k${SYMBOL_PREFIX}${stage_identifier}")
    set(data_symbol "${symbol}Data")
    list(FIND shader_symbols "${symbol}" duplicate_symbol_index)
    if (NOT duplicate_symbol_index EQUAL -1)
        message(FATAL_ERROR "Shader symbol is duplicated: ${symbol}")
    endif()
    list(APPEND shader_symbols "${symbol}")

    generate_spirv_array("${spv_file}" "${data_symbol}" array_text)
    string(APPEND source_content "${array_text}\n\n")
    string(APPEND header_content "    extern const ShaderBinary ${symbol};\n")
endforeach()

string(APPEND source_content "    } // namespace\n\n")

foreach(symbol IN LISTS shader_symbols)
    set(data_symbol "${symbol}Data")
    string(APPEND source_content "    const ShaderBinary ${symbol}{\n")
    string(APPEND source_content
        "            reinterpret_cast<const std::byte*>(${data_symbol}.data()),\n")
    string(APPEND source_content
        "            ${data_symbol}.size() * sizeof(std::uint32_t)\n")
    string(APPEND source_content "    };\n\n")
endforeach()

string(APPEND header_content "} // namespace ${NAMESPACE}\n")
string(APPEND source_content "} // namespace ${NAMESPACE}\n")

write_if_different("${OUTPUT_HEADER}" "${header_content}")
write_if_different("${OUTPUT_SOURCE}" "${source_content}")
