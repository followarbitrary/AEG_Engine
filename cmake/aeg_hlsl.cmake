#[[ Configure shared HLSL compilation and shader-program generation helpers. ]]
include(CMakeParseArguments)

get_filename_component(AEG_CXX_COMPILER_DIRECTORY
    "${CMAKE_CXX_COMPILER}" DIRECTORY)

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

#[[ Maps a shader filename suffix to a normalized stage and DXC profile. ]]

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

#[[ Converts a shader path key into generated program and symbol names. ]]

function(aeg_make_shader_program_names program_key name_output prefix_output package_output)
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
    set(${package_output} "${normalized_name}" PARENT_SCOPE)
endfunction()

#[[ Adds one DXC invocation for a single HLSL shader stage. ]]

function(aeg_add_hlsl_shader_stage)
    set(one_value_args
        OUTPUT
        OUTPUT_DIRECTORY
        PROFILE
        ENTRY_POINT
        SOURCE
        COMMENT
    )
    set(multi_value_args DEPENDENCIES HLSL_INCLUDE_DIRECTORIES)
    cmake_parse_arguments(ARG "" "${one_value_args}" "${multi_value_args}" ${ARGN})

    foreach(required_argument IN ITEMS OUTPUT OUTPUT_DIRECTORY PROFILE ENTRY_POINT SOURCE)
        if (NOT ARG_${required_argument})
            message(FATAL_ERROR
                "aeg_add_hlsl_shader_stage requires ${required_argument}")
        endif()
    endforeach()

    set(hlsl_include_options)
    foreach(include_directory IN LISTS ARG_HLSL_INCLUDE_DIRECTORIES)
        list(APPEND hlsl_include_options -I "${include_directory}")
    endforeach()

    add_custom_command(
        OUTPUT "${ARG_OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${ARG_OUTPUT_DIRECTORY}"
        COMMAND "${AEG_DXC_EXECUTABLE}"
            ${AEG_DXC_COMMON_OPTIONS}
            ${AEG_DXC_SPIRV_OPTIONS}
            ${hlsl_include_options}
            -T "${ARG_PROFILE}"
            -E "${ARG_ENTRY_POINT}"
            -Fo "${ARG_OUTPUT}"
            "${ARG_SOURCE}"
        DEPENDS
            "${ARG_SOURCE}"
            ${ARG_DEPENDENCIES}
            "${AEG_DXC_EXECUTABLE}"
        COMMENT "${ARG_COMMENT}"
        VERBATIM
    )
endfunction()

#[[ Adds a built-in shader program with generated C++ declarations and data. ]]

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
    set(multi_value_args STAGES DEPENDENCIES HLSL_INCLUDE_DIRECTORIES)
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
        aeg_add_hlsl_shader_stage(
            OUTPUT "${spv_output}"
            OUTPUT_DIRECTORY "${shader_output_directory}"
            PROFILE "${shader_profile}"
            ENTRY_POINT "${entry_point}"
            SOURCE "${shader_source}"
            DEPENDENCIES ${ARG_DEPENDENCIES}
            HLSL_INCLUDE_DIRECTORIES ${ARG_HLSL_INCLUDE_DIRECTORIES}
            COMMENT "Compiling ${ARG_NAME} ${stage_name} HLSL shader"
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
    target_include_directories("${ARG_TARGET}" PUBLIC
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

#[[ Adds a gameplay shader program that produces a reflected contract package. ]]

function(aeg_add_hlsl_gameplay_shader_program)
    set(one_value_args
        TARGET
        NAME
        OUTPUT_DIRECTORY
        CONTRACT_TOOL
        PACKAGE_OUTPUT_VARIABLE
    )
    set(multi_value_args STAGES DEPENDENCIES HLSL_INCLUDE_DIRECTORIES)
    cmake_parse_arguments(ARG "" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if (ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unknown arguments for aeg_add_hlsl_gameplay_shader_program: ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    foreach(required_argument IN ITEMS TARGET NAME OUTPUT_DIRECTORY CONTRACT_TOOL)
        if (NOT ARG_${required_argument})
            message(FATAL_ERROR
                "aeg_add_hlsl_gameplay_shader_program requires ${required_argument}")
        endif()
    endforeach()

    if (NOT TARGET "${ARG_TARGET}")
        message(FATAL_ERROR
            "Shader target does not exist: ${ARG_TARGET}")
    endif()
    if (NOT TARGET "${ARG_CONTRACT_TOOL}")
        message(FATAL_ERROR
            "Shader contract tool target does not exist: ${ARG_CONTRACT_TOOL}")
    endif()
    if (NOT ARG_STAGES)
        message(FATAL_ERROR
            "aeg_add_hlsl_gameplay_shader_program requires at least one stage")
    endif()

    set(shader_output_directory
        "${ARG_OUTPUT_DIRECTORY}/${ARG_NAME}")
    set(package_output
        "${ARG_OUTPUT_DIRECTORY}/${ARG_NAME}.aegshader")
    set(spv_outputs)
    set(shader_sources)
    set(stage_arguments)
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
        aeg_add_hlsl_shader_stage(
            OUTPUT "${spv_output}"
            OUTPUT_DIRECTORY "${shader_output_directory}"
            PROFILE "${shader_profile}"
            ENTRY_POINT "${entry_point}"
            SOURCE "${shader_source}"
            DEPENDENCIES ${ARG_DEPENDENCIES}
            HLSL_INCLUDE_DIRECTORIES ${ARG_HLSL_INCLUDE_DIRECTORIES}
            COMMENT "Compiling ${ARG_NAME} ${stage_name} HLSL shader"
        )

        list(APPEND spv_outputs "${spv_output}")
        list(APPEND shader_sources "${shader_source}")
        list(APPEND stage_names "${stage_name}")
        list(APPEND stage_arguments
            --stage "${stage_name}"
            --entry "${entry_point}"
            --input "${spv_output}"
        )
    endforeach()

    add_custom_command(
        OUTPUT "${package_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${ARG_OUTPUT_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" -E env
            "PATH=${AEG_CXX_COMPILER_DIRECTORY}"
            "$<TARGET_FILE:${ARG_CONTRACT_TOOL}>"
            --output "${package_output}"
            --name "${ARG_NAME}"
            ${stage_arguments}
        DEPENDS
            ${spv_outputs}
            ${shader_sources}
            ${ARG_DEPENDENCIES}
            "${ARG_CONTRACT_TOOL}"
        COMMENT "Building ${ARG_NAME} shader contract package"
        VERBATIM
    )

    target_sources("${ARG_TARGET}" PRIVATE
        "${package_output}"
        ${shader_sources}
    )
    add_dependencies("${ARG_TARGET}" "${ARG_CONTRACT_TOOL}")
    set_source_files_properties(
        "${package_output}"
        PROPERTIES GENERATED TRUE
    )
    set_source_files_properties(
        ${shader_sources}
        PROPERTIES HEADER_FILE_ONLY TRUE
    )

    if (ARG_PACKAGE_OUTPUT_VARIABLE)
        set(${ARG_PACKAGE_OUTPUT_VARIABLE} "${package_output}" PARENT_SCOPE)
    endif()
endfunction()

# Shader sources use <program>.<stage>.hlsl. Files without a recognized stage
# suffix are treated as shared HLSL include files and are not compiled alone.
# Shared includes can also live in a separate common shader directory.
#[[ Scans a shader directory and registers every supported shader program. ]]
function(aeg_add_hlsl_shader_directory)
    set(one_value_args
        TARGET
        SHADER_DIRECTORY
        COMMON_SHADER_DIRECTORY
        NAMESPACE
        OUTPUT_DIRECTORY
        GENERATED_INCLUDE_DIRECTORY
        GENERATED_SOURCE_DIRECTORY
        MODE
        CONTRACT_TOOL
        PACKAGE_OUTPUT_VARIABLE
    )
    cmake_parse_arguments(ARG "" "${one_value_args}" "" ${ARGN})

    if (ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unknown arguments for aeg_add_hlsl_shader_directory: ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    if (NOT ARG_MODE)
        set(ARG_MODE CORE)
    endif()
    string(TOUPPER "${ARG_MODE}" shader_mode)
    if (NOT shader_mode STREQUAL "CORE" AND NOT shader_mode STREQUAL "GAMEPLAY")
        message(FATAL_ERROR
            "aeg_add_hlsl_shader_directory MODE must be CORE or GAMEPLAY")
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
    if (ARG_COMMON_SHADER_DIRECTORY AND
        NOT IS_DIRECTORY "${ARG_COMMON_SHADER_DIRECTORY}")
        message(FATAL_ERROR
            "Common shader directory does not exist: ${ARG_COMMON_SHADER_DIRECTORY}")
    endif()
    if (shader_mode STREQUAL "GAMEPLAY" AND NOT ARG_CONTRACT_TOOL)
        message(FATAL_ERROR
            "GAMEPLAY shader directories require CONTRACT_TOOL")
    endif()

    file(GLOB_RECURSE shared_shader_sources
        CONFIGURE_DEPENDS
        "${ARG_SHADER_DIRECTORY}/*.hlsl"
    )
    set(shared_shader_includes)
    foreach(shader_source IN LISTS shared_shader_sources)
        get_filename_component(shader_file_name "${shader_source}" NAME)
        if (shader_file_name MATCHES "^(.+)\.([A-Za-z0-9_]+)\.hlsl$")
            aeg_get_hlsl_stage_info("${CMAKE_MATCH_2}" recognized_stage recognized_profile)
            if (recognized_stage)
                continue()
            endif()
        endif()
        list(APPEND shared_shader_includes "${shader_source}")
    endforeach()

    if (ARG_COMMON_SHADER_DIRECTORY)
        file(GLOB_RECURSE common_shader_sources
            CONFIGURE_DEPENDS
            "${ARG_COMMON_SHADER_DIRECTORY}/*.hlsl"
        )
        list(APPEND shared_shader_includes ${common_shader_sources})
    endif()
    list(REMOVE_DUPLICATES shared_shader_includes)

    set(shader_include_directories
        "${ARG_SHADER_DIRECTORY}")
    if (ARG_COMMON_SHADER_DIRECTORY)
        list(APPEND shader_include_directories
            "${ARG_COMMON_SHADER_DIRECTORY}")
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
        if (NOT program_stem MATCHES "^[a-z][a-z0-9]*(_[a-z0-9]+)*$")
            message(FATAL_ERROR
                "Shader program name must use lowercase snake_case: ${shader_file_name}")
        endif()
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
            "${program_key}" program_name symbol_prefix package_name)

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
        set(package_name_variable
            "aeg_hlsl_package_name_${symbol_prefix}")
        set(${package_name_variable} "${package_name}")

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
        if (shader_mode STREQUAL "CORE")
            aeg_add_hlsl_shader_program(
                TARGET "${ARG_TARGET}"
                NAME "${program_name}"
                NAMESPACE "${ARG_NAMESPACE}"
                OUTPUT_DIRECTORY "${ARG_OUTPUT_DIRECTORY}"
                GENERATED_INCLUDE_DIRECTORY "${ARG_GENERATED_INCLUDE_DIRECTORY}"
                GENERATED_SOURCE_DIRECTORY "${ARG_GENERATED_SOURCE_DIRECTORY}"
                SYMBOL_PREFIX "${symbol_prefix}"
                STAGES ${stage_specs}
                DEPENDENCIES ${shared_shader_includes}
                HLSL_INCLUDE_DIRECTORIES ${shader_include_directories}
            )
        else()
            aeg_add_hlsl_gameplay_shader_program(
                TARGET "${ARG_TARGET}"
                NAME "${${package_name_variable}}"
                OUTPUT_DIRECTORY "${ARG_OUTPUT_DIRECTORY}"
                CONTRACT_TOOL "${ARG_CONTRACT_TOOL}"
                STAGES ${stage_specs}
                DEPENDENCIES ${shared_shader_includes}
                HLSL_INCLUDE_DIRECTORIES ${shader_include_directories}
                PACKAGE_OUTPUT_VARIABLE package_output
            )
            list(APPEND package_outputs "${package_output}")
        endif()
    endforeach()

    if (ARG_PACKAGE_OUTPUT_VARIABLE)
        set(${ARG_PACKAGE_OUTPUT_VARIABLE} "${package_outputs}" PARENT_SCOPE)
    endif()
endfunction()
