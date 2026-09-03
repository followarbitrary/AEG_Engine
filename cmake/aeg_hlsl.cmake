include(CMakeParseArguments)

set(AEG_HLSL_GENERATOR_SCRIPT
    "${CMAKE_CURRENT_LIST_DIR}/generate_shader_sources.cmake")

if (NOT DEFINED AEG_DXC_COMMON_OPTIONS)
    set(AEG_DXC_COMMON_OPTIONS
        -HV 2021
        -Ges
        -WX
        -Zpc
        -O3
    )
endif()

if (NOT DEFINED AEG_DXC_SPIRV_OPTIONS)
    set(AEG_DXC_SPIRV_OPTIONS
        -spirv
        -fspv-target-env=vulkan1.3
        -fvk-use-dx-layout
    )
endif()

function(aeg_get_hlsl_stage_info stage_suffix stage_output profile_output)
    string(TOLOWER "${stage_suffix}" normalized_suffix)

    set(stage_name)
    set(shader_profile)
    if (normalized_suffix STREQUAL "vert" OR
        normalized_suffix STREQUAL "vertex" OR
        normalized_suffix STREQUAL "vs")
        set(stage_name vertex)
        set(shader_profile vs_6_0)
    elseif (normalized_suffix STREQUAL "frag" OR
            normalized_suffix STREQUAL "fragment" OR
            normalized_suffix STREQUAL "pixel" OR
            normalized_suffix STREQUAL "ps")
        set(stage_name fragment)
        set(shader_profile ps_6_0)
    elseif (normalized_suffix STREQUAL "comp" OR
            normalized_suffix STREQUAL "compute" OR
            normalized_suffix STREQUAL "cs")
        set(stage_name compute)
        set(shader_profile cs_6_0)
    elseif (normalized_suffix STREQUAL "geom" OR
            normalized_suffix STREQUAL "geometry" OR
            normalized_suffix STREQUAL "gs")
        set(stage_name geometry)
        set(shader_profile gs_6_0)
    elseif (normalized_suffix STREQUAL "tesc" OR
            normalized_suffix STREQUAL "hull" OR
            normalized_suffix STREQUAL "hs")
        set(stage_name tessellation_control)
        set(shader_profile hs_6_0)
    elseif (normalized_suffix STREQUAL "tese" OR
            normalized_suffix STREQUAL "domain" OR
            normalized_suffix STREQUAL "ds")
        set(stage_name tessellation_evaluation)
        set(shader_profile ds_6_0)
    elseif (normalized_suffix STREQUAL "mesh" OR
            normalized_suffix STREQUAL "ms")
        set(stage_name mesh)
        set(shader_profile ms_6_5)
    endif()

    set(${stage_output} "${stage_name}" PARENT_SCOPE)
    set(${profile_output} "${shader_profile}" PARENT_SCOPE)
endfunction()

function(aeg_make_shader_program_names program_key name_output prefix_output)
    string(REGEX REPLACE "[^A-Za-z0-9]+" "_" normalized_name "${program_key}")
    string(REGEX REPLACE "_+" "_" normalized_name "${normalized_name}")
    string(REGEX REPLACE "^_+" "" normalized_name "${normalized_name}")
    string(REGEX REPLACE "_+$" "" normalized_name "${normalized_name}")
    if (NOT normalized_name)
        message(FATAL_ERROR
            "Shader program name is empty after normalization: ${program_key}")
    endif()
    if (normalized_name MATCHES "^[0-9]")
        set(normalized_name "shader_${normalized_name}")
    endif()

    set(program_name "${normalized_name}_shaders")
    string(REPLACE "_" ";" name_parts "${normalized_name}")
    set(symbol_prefix)
    foreach(name_part IN LISTS name_parts)
        if (NOT name_part)
            continue()
        endif()
        string(SUBSTRING "${name_part}" 0 1 first_character)
        string(TOUPPER "${first_character}" first_character_upper)
        string(SUBSTRING "${name_part}" 1 -1 remaining_characters)
        string(APPEND symbol_prefix "${first_character_upper}${remaining_characters}")
    endforeach()

    set(${name_output} "${program_name}" PARENT_SCOPE)
    set(${prefix_output} "${symbol_prefix}" PARENT_SCOPE)
endfunction()

function(aeg_add_hlsl_shader_program)
    set(one_value_args
        TARGET
        NAME
        NAMESPACE
        OUTPUT_DIRECTORY
        GENERATED_INCLUDE_DIRECTORY
        GENERATED_SOURCE_DIRECTORY
        SYMBOL_PREFIX
    )
    set(multi_value_args STAGES)
    cmake_parse_arguments(ARG "" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if (ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unknown arguments for aeg_add_hlsl_shader_program: ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    foreach(required_argument IN ITEMS
            TARGET NAME NAMESPACE OUTPUT_DIRECTORY GENERATED_INCLUDE_DIRECTORY
            GENERATED_SOURCE_DIRECTORY SYMBOL_PREFIX)
        if (NOT ARG_${required_argument})
            message(FATAL_ERROR
                "aeg_add_hlsl_shader_program requires ${required_argument}")
        endif()
    endforeach()

    if (NOT TARGET "${ARG_TARGET}")
        message(FATAL_ERROR
            "Shader target does not exist: ${ARG_TARGET}")
    endif()
    if (NOT ARG_STAGES)
        message(FATAL_ERROR
            "aeg_add_hlsl_shader_program requires at least one stage")
    endif()
    if (NOT ARG_SYMBOL_PREFIX MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR
            "Shader symbol prefix is not a valid identifier: ${ARG_SYMBOL_PREFIX}")
    endif()

    set(generated_header
        "${ARG_GENERATED_INCLUDE_DIRECTORY}/${ARG_NAME}.hpp")
    set(generated_source
        "${ARG_GENERATED_SOURCE_DIRECTORY}/${ARG_NAME}.cpp")
    set(shader_output_directory
        "${ARG_OUTPUT_DIRECTORY}/${ARG_NAME}")
    set(generator_script
        "${AEG_HLSL_GENERATOR_SCRIPT}")
    set(spv_outputs)
    set(shader_sources)
    set(shader_specs)
    set(stage_names)

    foreach(stage_spec IN LISTS ARG_STAGES)
        string(REPLACE "|" ";" stage_fields "${stage_spec}")
        list(LENGTH stage_fields stage_field_count)
        if (stage_field_count LESS 3 OR stage_field_count GREATER 4)
            message(FATAL_ERROR
                "Shader stage must be stage|profile|source[|entry]: ${stage_spec}")
        endif()

        list(GET stage_fields 0 stage_name)
        list(GET stage_fields 1 shader_profile)
        list(GET stage_fields 2 shader_source)
        if (stage_field_count EQUAL 4)
            list(GET stage_fields 3 entry_point)
        else()
            set(entry_point main)
        endif()

        if (NOT stage_name MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
            message(FATAL_ERROR "Shader stage name is not a valid identifier: ${stage_name}")
        endif()
        if (NOT EXISTS "${shader_source}")
            message(FATAL_ERROR "HLSL shader source does not exist: ${shader_source}")
        endif()

        list(FIND stage_names "${stage_name}" duplicate_stage_index)
        if (NOT duplicate_stage_index EQUAL -1)
            message(FATAL_ERROR "Shader program contains duplicate stage: ${stage_name}")
        endif()

        set(spv_output "${shader_output_directory}/${stage_name}.spv")
        add_custom_command(
            OUTPUT "${spv_output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${shader_output_directory}"
            COMMAND "${AEG_DXC_EXECUTABLE}"
                ${AEG_DXC_COMMON_OPTIONS}
                ${AEG_DXC_SPIRV_OPTIONS}
                -T "${shader_profile}"
                -E "${entry_point}"
                -Fo "${spv_output}"
                "${shader_source}"
            DEPENDS
                "${shader_source}"
                "${AEG_DXC_EXECUTABLE}"
            COMMENT "Compiling ${ARG_NAME} ${stage_name} HLSL shader"
            VERBATIM
        )

        list(APPEND spv_outputs "${spv_output}")
        list(APPEND shader_sources "${shader_source}")
        list(APPEND stage_names "${stage_name}")
        list(APPEND shader_specs "${stage_name}|${spv_output}|${entry_point}")
    endforeach()

    add_custom_command(
        OUTPUT
            "${generated_header}"
            "${generated_source}"
        COMMAND "${CMAKE_COMMAND}"
            "-DOUTPUT_HEADER=${generated_header}"
            "-DOUTPUT_SOURCE=${generated_source}"
            "-DNAMESPACE=${ARG_NAMESPACE}"
            "-DSYMBOL_PREFIX=${ARG_SYMBOL_PREFIX}"
            "-DSHADER_SPECS=${shader_specs}"
            -P "${generator_script}"
        DEPENDS
            ${spv_outputs}
            "${generator_script}"
        COMMENT "Generating ${ARG_NAME} C++ shader interface and data"
        VERBATIM
    )

    target_sources("${ARG_TARGET}" PRIVATE
        "${generated_header}"
        "${generated_source}"
        ${shader_sources}
    )
    get_filename_component(shader_public_directory
        "${ARG_GENERATED_INCLUDE_DIRECTORY}" DIRECTORY)
    target_include_directories("${ARG_TARGET}" PRIVATE
        "${ARG_GENERATED_INCLUDE_DIRECTORY}"
        "${shader_public_directory}"
    )
    set_source_files_properties(
        "${generated_header}"
        "${generated_source}"
        PROPERTIES GENERATED TRUE
    )
    set_source_files_properties(
        ${shader_sources}
        PROPERTIES HEADER_FILE_ONLY TRUE
    )
endfunction()

# Shader sources use <program>.<stage>.hlsl. Files without a recognized stage
# suffix are treated as shared HLSL include files and are not compiled alone.
function(aeg_add_hlsl_shader_directory)
    set(one_value_args
        TARGET
        SHADER_DIRECTORY
        NAMESPACE
        OUTPUT_DIRECTORY
        GENERATED_INCLUDE_DIRECTORY
        GENERATED_SOURCE_DIRECTORY
    )
    cmake_parse_arguments(ARG "" "${one_value_args}" "" ${ARGN})

    if (ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unknown arguments for aeg_add_hlsl_shader_directory: ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    foreach(required_argument IN ITEMS
            TARGET SHADER_DIRECTORY NAMESPACE OUTPUT_DIRECTORY GENERATED_INCLUDE_DIRECTORY
            GENERATED_SOURCE_DIRECTORY)
        if (NOT ARG_${required_argument})
            message(FATAL_ERROR
                "aeg_add_hlsl_shader_directory requires ${required_argument}")
        endif()
    endforeach()

    if (NOT TARGET "${ARG_TARGET}")
        message(FATAL_ERROR
            "Shader target does not exist: ${ARG_TARGET}")
    endif()
    if (NOT IS_DIRECTORY "${ARG_SHADER_DIRECTORY}")
        message(FATAL_ERROR
            "Shader directory does not exist: ${ARG_SHADER_DIRECTORY}")
    endif()

    file(GLOB_RECURSE shader_sources
        CONFIGURE_DEPENDS
        "${ARG_SHADER_DIRECTORY}/*.hlsl"
    )
    list(SORT shader_sources)

    set(program_identifiers)
    foreach(shader_source IN LISTS shader_sources)
        get_filename_component(shader_file_name "${shader_source}" NAME)
        if (NOT shader_file_name MATCHES "^(.+)\\.([A-Za-z0-9_]+)\\.hlsl$")
            continue()
        endif()

        set(program_stem "${CMAKE_MATCH_1}")
        set(stage_suffix "${CMAKE_MATCH_2}")
        aeg_get_hlsl_stage_info("${stage_suffix}" stage_name shader_profile)
        if (NOT stage_name)
            continue()
        endif()

        file(RELATIVE_PATH relative_shader_source
            "${ARG_SHADER_DIRECTORY}"
            "${shader_source}")
        get_filename_component(relative_shader_directory
            "${relative_shader_source}" DIRECTORY)
        if (relative_shader_directory STREQUAL "." OR
            relative_shader_directory STREQUAL "")
            set(program_key "${program_stem}")
        else()
            set(program_key "${relative_shader_directory}/${program_stem}")
        endif()

        aeg_make_shader_program_names(
            "${program_key}" program_name symbol_prefix)

        set(program_key_variable
            "aeg_hlsl_program_key_${symbol_prefix}")
        if (DEFINED ${program_key_variable})
            if (NOT "${${program_key_variable}}" STREQUAL "${program_key}")
                message(FATAL_ERROR
                    "Shader programs normalize to the same identifier: "
                    "${${program_key_variable}} and ${program_key}")
            endif()
        else()
            set(${program_key_variable} "${program_key}")
        endif()
        set(program_name_variable
            "aeg_hlsl_program_name_${symbol_prefix}")
        set(${program_name_variable} "${program_name}")

        set(stage_specs_variable
            "aeg_hlsl_stage_specs_${symbol_prefix}")
        set(stage_specs "${${stage_specs_variable}}")
        list(APPEND stage_specs
            "${stage_name}|${shader_profile}|${shader_source}")
        set(${stage_specs_variable} "${stage_specs}")
        list(APPEND program_identifiers "${symbol_prefix}")
    endforeach()

    list(REMOVE_DUPLICATES program_identifiers)
    if (NOT program_identifiers)
        message(FATAL_ERROR
            "No supported HLSL shader stages were found in ${ARG_SHADER_DIRECTORY}")
    endif()

    foreach(symbol_prefix IN LISTS program_identifiers)
        set(stage_specs_variable
            "aeg_hlsl_stage_specs_${symbol_prefix}")
        set(stage_specs "${${stage_specs_variable}}")
        list(SORT stage_specs)

        set(program_name_variable
            "aeg_hlsl_program_name_${symbol_prefix}")
        set(program_name "${${program_name_variable}}")
        aeg_add_hlsl_shader_program(
            TARGET "${ARG_TARGET}"
            NAME "${program_name}"
            NAMESPACE "${ARG_NAMESPACE}"
            OUTPUT_DIRECTORY "${ARG_OUTPUT_DIRECTORY}"
            GENERATED_INCLUDE_DIRECTORY "${ARG_GENERATED_INCLUDE_DIRECTORY}"
            GENERATED_SOURCE_DIRECTORY "${ARG_GENERATED_SOURCE_DIRECTORY}"
            SYMBOL_PREFIX "${symbol_prefix}"
            STAGES ${stage_specs}
        )
    endforeach()
endfunction()
