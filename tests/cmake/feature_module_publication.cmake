cmake_minimum_required(VERSION 3.25)
foreach(required ENGINE_DIR TEST_ROOT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Module publication smoke requires ${required}")
    endif()
endforeach()
get_filename_component(ENGINE_DIR "${ENGINE_DIR}" ABSOLUTE)
get_filename_component(TEST_ROOT "${TEST_ROOT}" ABSOLUTE)
get_filename_component(test_name "${TEST_ROOT}" NAME)
if(NOT test_name STREQUAL "feature-module-publication")
    message(FATAL_ERROR "Unexpected module publication fixture root: ${TEST_ROOT}")
endif()
# This test owns only its explicitly named generated tree.
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(PROJECT_ROOT "${TEST_ROOT}/Source Project")
set(BUILD_ROOT "${TEST_ROOT}/Build")
file(COPY "${ENGINE_DIR}/tests/fixtures/module_publication/" DESTINATION "${PROJECT_ROOT}")
file(COPY "${ENGINE_DIR}/tests/fixtures/contract_exporter/Source/" DESTINATION "${PROJECT_ROOT}/Source")

function(run_logged label)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 420)
    file(WRITE "${TEST_ROOT}/${label}.log" "exit=${result}\n${output}\n${error}")
    if(NOT "${result}" STREQUAL "0")
        message(FATAL_ERROR "Fixture setup ${label} failed (${result}):\n${output}\n${error}")
    endif()
endfunction()

function(find_executable output name base)
    foreach(candidate "${base}/Debug/${name}.exe" "${base}/${name}.exe" "${base}/${name}")
        if(EXISTS "${candidate}")
            set(${output} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "Fixture did not build ${name} below ${base}")
endfunction()

set(generator_options)
if(DEFINED GENERATOR AND NOT GENERATOR STREQUAL "")
    list(APPEND generator_options -G "${GENERATOR}")
endif()
if(DEFINED GENERATOR_PLATFORM AND NOT GENERATOR_PLATFORM STREQUAL "")
    list(APPEND generator_options -A "${GENERATOR_PLATFORM}")
endif()
if(DEFINED GENERATOR_TOOLSET AND NOT GENERATOR_TOOLSET STREQUAL "")
    list(APPEND generator_options -T "${GENERATOR_TOOLSET}")
endif()
run_logged(configure "${CMAKE_COMMAND}" -S "${PROJECT_ROOT}" -B "${BUILD_ROOT}" ${generator_options}
    "-DENGINE_DIR=${ENGINE_DIR}" -DCMAKE_BUILD_TYPE=Debug -DPUBLICATION_BUILD_ID=publication-v1)
run_logged(build-baseline "${CMAKE_COMMAND}" --build "${BUILD_ROOT}" --config Debug
    --target PublicationFixture PublicationVerifier --parallel 4)
find_executable(VERIFIER PublicationVerifier "${BUILD_ROOT}")
find_executable(EXPORTER PublicationFixture_contract_exporter "${BUILD_ROOT}")
find_executable(METADATA_TOOL aeg_module_metadata_tool "${BUILD_ROOT}/aeg-engine")
set(BUNDLE "${BUILD_ROOT}/ModuleBundle/Debug")
set(MANIFEST "${BUNDLE}/module.manifest.json")
set(SOURCE_METADATA "${BUILD_ROOT}/generated/module-contracts/PublicationFixture/Debug/module.source.metadata.json")
run_logged(verify-baseline "${VERIFIER}" --verify "${MANIFEST}" publication-v1 windows-x64-vulkan 1)
file(READ "${MANIFEST}" baseline_manifest)
string(JSON baseline_binary_name GET "${baseline_manifest}" binary_path)
string(JSON baseline_metadata_name GET "${baseline_manifest}" metadata_path)
get_filename_component(module_suffix "${baseline_binary_name}" LAST_EXT)
file(SHA256 "${MANIFEST}" baseline_manifest_sha)
file(SHA256 "${BUNDLE}/${baseline_binary_name}" baseline_binary_sha)
file(SHA256 "${BUNDLE}/${baseline_metadata_name}" baseline_metadata_sha)
file(MAKE_DIRECTORY "${TEST_ROOT}/BaselineGuard")
file(COPY_FILE "${MANIFEST}" "${TEST_ROOT}/BaselineGuard/manifest")
file(COPY_FILE "${BUNDLE}/${baseline_binary_name}" "${TEST_ROOT}/BaselineGuard/binary")
file(COPY_FILE "${BUNDLE}/${baseline_metadata_name}" "${TEST_ROOT}/BaselineGuard/metadata")
file(COPY_FILE "${SOURCE_METADATA}" "${TEST_ROOT}/BaselineGuard/source-metadata")

set(failures 0)

# Publication failure: a valid candidate reaches the real final-selector write.
# On Windows hold the actual active selector readable, but deny its replacement.
run_logged(export-candidate "${EXPORTER}" --output "${TEST_ROOT}/candidate.metadata.json"
    --build-id publication-candidate --target windows-x64-vulkan --config Debug)
file(COPY_FILE "${BUNDLE}/${baseline_binary_name}" "${BUNDLE}/CandidateModule${module_suffix}")
set(candidate_binary "${BUNDLE}/CandidateModule${module_suffix}")
if(WIN32)
    execute_process(COMMAND "${VERIFIER}" --locked-publish "${METADATA_TOOL}" "${candidate_binary}"
        "${TEST_ROOT}/candidate.metadata.json" "${BUNDLE}/module.metadata.json" "${MANIFEST}"
        publication-candidate windows-x64-vulkan Debug
        RESULT_VARIABLE locked_result OUTPUT_VARIABLE publication_output ERROR_VARIABLE publication_error TIMEOUT 60)
    if(NOT "${locked_result}" STREQUAL "0" OR
       NOT publication_output MATCHES "PUBLICATION_LOCKED_CHILD_EXIT (-?[0-9]+)")
        message(FATAL_ERROR "Publication lock fixture did not run its actual child: ${locked_result}\n${publication_output}\n${publication_error}")
    endif()
    set(publication_result "${CMAKE_MATCH_1}")
else()
    # A nonempty directory is a deterministic unreplaceable selector. Its sibling
    # active selector must retain all referenced payloads even if this publish fails.
    file(MAKE_DIRECTORY "${BUNDLE}/BlockedSelector")
    file(WRITE "${BUNDLE}/BlockedSelector/keep" "owned fixture sentinel")
    execute_process(COMMAND "${METADATA_TOOL}" --module "${candidate_binary}"
        --metadata "${TEST_ROOT}/candidate.metadata.json" --output "${BUNDLE}/module.metadata.json"
        --manifest "${BUNDLE}/BlockedSelector" --build-id publication-candidate
        --target windows-x64-vulkan --config Debug
        RESULT_VARIABLE publication_result OUTPUT_VARIABLE publication_output ERROR_VARIABLE publication_error TIMEOUT 60)
endif()
file(WRITE "${TEST_ROOT}/publication-failure.log"
    "native-publisher-exit=${publication_result}\n${publication_output}\n${publication_error}")
set(retained TRUE)
foreach(item manifest binary metadata)
    if(item STREQUAL "manifest")
        set(path "${MANIFEST}")
    elseif(item STREQUAL "binary")
        set(path "${BUNDLE}/${baseline_binary_name}")
    else()
        set(path "${BUNDLE}/${baseline_metadata_name}")
    endif()
    if(NOT EXISTS "${path}")
        set(retained FALSE)
    else()
        file(SHA256 "${path}" retained_sha)
        if(NOT "${retained_sha}" STREQUAL "${baseline_${item}_sha}")
            set(retained FALSE)
        endif()
    endif()
endforeach()
execute_process(COMMAND "${VERIFIER}" --verify "${MANIFEST}" publication-v1 windows-x64-vulkan 1
    RESULT_VARIABLE retained_result OUTPUT_VARIABLE retained_output ERROR_VARIABLE retained_error TIMEOUT 60)
file(APPEND "${TEST_ROOT}/publication-failure.log" "retained-hashes=${retained}\n${retained_output}\n${retained_error}")
# Existing CLI code 13 identifies its final publication/write failure. A parse,
# metadata-validation or launch error must not masquerade as this negative case.
if(NOT "${publication_result}" STREQUAL "13" OR NOT retained OR NOT "${retained_result}" STREQUAL "0")
    set(direct_publication_passed FALSE)
else()
    set(direct_publication_passed TRUE)
endif()

# Restore only this test's disposable artifacts after recording every failure;
# later independent cases must not inherit damage from the old publisher.
file(COPY_FILE "${TEST_ROOT}/BaselineGuard/manifest" "${MANIFEST}")
file(COPY_FILE "${TEST_ROOT}/BaselineGuard/binary" "${BUNDLE}/${baseline_binary_name}")
file(COPY_FILE "${TEST_ROOT}/BaselineGuard/metadata" "${BUNDLE}/${baseline_metadata_name}")
run_logged(verify-restored-baseline "${VERIFIER}" --verify "${MANIFEST}" publication-v1 windows-x64-vulkan 1)

# Exercise the actual CMake copy/publication chain too: its candidate DLL really
# changes. A tool-only metadata failure cannot prove that a preceding fixed-path
# DLL copy retained the last-good binary.
set(cmake_publication_passed TRUE)
if(WIN32)
    set(revision_file "${PROJECT_ROOT}/Source/Revision.hpp")
    file(READ "${revision_file}" revision_v1)
    string(REPLACE "kRevision = 1;" "kRevision = 2;" revision_v2 "${revision_v1}")
    if(revision_v1 STREQUAL revision_v2)
        message(FATAL_ERROR "Publication failure fixture did not change its actual DLL revision")
    endif()
    file(WRITE "${revision_file}" "${revision_v2}")
    execute_process(COMMAND "${VERIFIER}" --locked-build "${CMAKE_COMMAND}" "${BUILD_ROOT}" "${MANIFEST}" PublicationFixture
        RESULT_VARIABLE locked_build_result OUTPUT_VARIABLE failed_build_output ERROR_VARIABLE failed_build_error TIMEOUT 420)
    if(NOT "${locked_build_result}" STREQUAL "0" OR
       NOT failed_build_output MATCHES "PUBLICATION_LOCKED_CHILD_EXIT (-?[0-9]+)")
        message(FATAL_ERROR "CMake publication lock probe did not launch its real child: ${locked_build_result}\n${failed_build_output}\n${failed_build_error}")
    endif()
    set(failed_build_result "${CMAKE_MATCH_1}")
    file(WRITE "${TEST_ROOT}/cmake-publication-failure.log"
        "native-build-exit=${failed_build_result}\n${failed_build_output}\n${failed_build_error}")
    set(build_retained TRUE)
    foreach(item manifest binary metadata)
        if(item STREQUAL "manifest")
            set(path "${MANIFEST}")
        elseif(item STREQUAL "binary")
            set(path "${BUNDLE}/${baseline_binary_name}")
        else()
            set(path "${BUNDLE}/${baseline_metadata_name}")
        endif()
        if(NOT EXISTS "${path}")
            set(build_retained FALSE)
        else()
            file(SHA256 "${path}" actual_sha)
            if(NOT "${actual_sha}" STREQUAL "${baseline_${item}_sha}")
                set(build_retained FALSE)
            endif()
        endif()
    endforeach()
    execute_process(COMMAND "${VERIFIER}" --verify "${MANIFEST}" publication-v1 windows-x64-vulkan 1
        RESULT_VARIABLE build_retained_result OUTPUT_VARIABLE build_retained_output ERROR_VARIABLE build_retained_error TIMEOUT 60)
    file(APPEND "${TEST_ROOT}/cmake-publication-failure.log"
        "retained-hashes=${build_retained}\n${build_retained_output}\n${build_retained_error}")
    # Require the real publisher's final write diagnostic. Compiler/argument
    # failures and factory rejection cannot satisfy this failure oracle.
    if("${failed_build_result}" STREQUAL "0" OR NOT build_retained OR
       NOT "${build_retained_result}" STREQUAL "0" OR
       NOT "${failed_build_output}\n${failed_build_error}" MATCHES "could not write staged metadata bundle")
        set(cmake_publication_passed FALSE)
    endif()
    file(COPY_FILE "${TEST_ROOT}/BaselineGuard/manifest" "${MANIFEST}")
    file(COPY_FILE "${TEST_ROOT}/BaselineGuard/binary" "${BUNDLE}/${baseline_binary_name}")
    file(COPY_FILE "${TEST_ROOT}/BaselineGuard/metadata" "${BUNDLE}/${baseline_metadata_name}")
    file(WRITE "${revision_file}" "${revision_v1}")
    run_logged(rebuild-restored-source "${CMAKE_COMMAND}" --build "${BUILD_ROOT}" --config Debug
        --target PublicationFixture --parallel 4)
    run_logged(verify-restored-source "${VERIFIER}" --verify "${MANIFEST}" publication-v1 windows-x64-vulkan 1)
endif()
if(NOT direct_publication_passed OR NOT cmake_publication_passed)
    math(EXPR failures "${failures}+1")
    message(STATUS "PUBLICATION_FAILURE_RETAINS_LAST_GOOD passed=0 direct=${direct_publication_passed} cmake-chain=${cmake_publication_passed} native-tool-exit=${publication_result}")
else()
    message(STATUS "PUBLICATION_FAILURE_RETAINS_LAST_GOOD passed=1")
endif()

# Existing METADATA form: a metadata-only invalid digest must reach publication
# validation. It must fail the build and preserve the last-good bundle; an up-to-
# date DLL cannot silently cause the changed input to be ignored.
file(COPY_FILE "${TEST_ROOT}/BaselineGuard/source-metadata" "${PROJECT_ROOT}/OnlyMetadata.json")
run_logged(build-explicit-metadata-baseline "${CMAKE_COMMAND}" --build "${BUILD_ROOT}" --config Debug
    --target PublicationMetadataOnlyFixture --parallel 4)
set(explicit_bundle "${BUILD_ROOT}/MetadataOnlyBundle/Debug")
set(explicit_manifest "${explicit_bundle}/module.manifest.json")
run_logged(verify-explicit-metadata-baseline "${VERIFIER}" --verify "${explicit_manifest}"
    publication-v1 windows-x64-vulkan 1)
file(READ "${explicit_manifest}" explicit_manifest_bytes)
string(JSON explicit_binary_name GET "${explicit_manifest_bytes}" binary_path)
string(JSON explicit_metadata_name GET "${explicit_manifest_bytes}" metadata_path)
file(SHA256 "${explicit_manifest}" explicit_manifest_sha)
file(SHA256 "${explicit_bundle}/${explicit_binary_name}" explicit_binary_sha)
file(SHA256 "${explicit_bundle}/${explicit_metadata_name}" explicit_metadata_sha)
file(READ "${PROJECT_ROOT}/OnlyMetadata.json" invalid_metadata)
string(JSON invalid_metadata SET "${invalid_metadata}" contract_digest
    "\"0000000000000000000000000000000000000000000000000000000000000000\"")
file(WRITE "${PROJECT_ROOT}/OnlyMetadata.json" "${invalid_metadata}\n")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD_ROOT}" --config Debug
    --target PublicationMetadataOnlyFixture --parallel 4
    RESULT_VARIABLE explicit_result OUTPUT_VARIABLE explicit_output ERROR_VARIABLE explicit_error TIMEOUT 420)
file(WRITE "${TEST_ROOT}/metadata-only-invalid-input.log" "exit=${explicit_result}\n${explicit_output}\n${explicit_error}")
set(explicit_retained TRUE)
foreach(item manifest binary metadata)
    if(item STREQUAL "manifest")
        set(path "${explicit_manifest}")
    elseif(item STREQUAL "binary")
        set(path "${explicit_bundle}/${explicit_binary_name}")
    else()
        set(path "${explicit_bundle}/${explicit_metadata_name}")
    endif()
    if(NOT EXISTS "${path}")
        set(explicit_retained FALSE)
    else()
        file(SHA256 "${path}" actual_sha)
        if(NOT "${actual_sha}" STREQUAL "${explicit_${item}_sha}")
            set(explicit_retained FALSE)
        endif()
    endif()
endforeach()
execute_process(COMMAND "${VERIFIER}" --verify "${explicit_manifest}" publication-v1 windows-x64-vulkan 1
    RESULT_VARIABLE explicit_verify_result OUTPUT_VARIABLE explicit_verify_output ERROR_VARIABLE explicit_verify_error TIMEOUT 60)
file(APPEND "${TEST_ROOT}/metadata-only-invalid-input.log"
    "retained-hashes=${explicit_retained}\n${explicit_verify_output}\n${explicit_verify_error}")
set(explicit_checked FALSE)
if(NOT "${explicit_result}" STREQUAL "0" AND explicit_retained AND
   "${explicit_verify_result}" STREQUAL "0" AND "${explicit_output}\n${explicit_error}" MATCHES "[Dd]igest")
    set(explicit_checked TRUE)
endif()

# Factory form: no C++ source or contract is edited. Building the public module
# target must publish a new identity even if its DLL objects are up to date.
file(SHA256 "${PROJECT_ROOT}/Source/ModuleFactory.cpp" factory_before)
run_logged(reconfigure-metadata-only "${CMAKE_COMMAND}" -S "${PROJECT_ROOT}" -B "${BUILD_ROOT}"
    "-DENGINE_DIR=${ENGINE_DIR}" -DCMAKE_BUILD_TYPE=Debug -DPUBLICATION_BUILD_ID=publication-v2)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD_ROOT}" --config Debug --target PublicationFixture --parallel 4
    RESULT_VARIABLE metadata_build_result OUTPUT_VARIABLE metadata_build_output ERROR_VARIABLE metadata_build_error TIMEOUT 420)
file(WRITE "${TEST_ROOT}/metadata-only-build.log" "exit=${metadata_build_result}\n${metadata_build_output}\n${metadata_build_error}")
file(SHA256 "${PROJECT_ROOT}/Source/ModuleFactory.cpp" factory_after)
execute_process(COMMAND "${VERIFIER}" --verify "${MANIFEST}" publication-v2 windows-x64-vulkan 1
    RESULT_VARIABLE metadata_verify_result OUTPUT_VARIABLE metadata_verify_output ERROR_VARIABLE metadata_verify_error TIMEOUT 60)
file(WRITE "${TEST_ROOT}/metadata-only-verify.log" "exit=${metadata_verify_result}\n${metadata_verify_output}\n${metadata_verify_error}")
file(READ "${SOURCE_METADATA}" metadata_source_after)
string(JSON exported_identity GET "${metadata_source_after}" build_id)
if(NOT explicit_checked OR NOT "${metadata_build_result}" STREQUAL "0" OR NOT "${metadata_verify_result}" STREQUAL "0" OR
   NOT exported_identity STREQUAL "publication-v2" OR NOT factory_before STREQUAL factory_after)
    math(EXPR failures "${failures}+1")
    message(STATUS "METADATA_ONLY_REPUBLISH passed=0 explicit-input-checked=${explicit_checked} build=${metadata_build_result} verify=${metadata_verify_result} source-id=${exported_identity}")
else()
    message(STATUS "METADATA_ONLY_REPUBLISH passed=1")
endif()

# Neither the CMake script nor the verifier narrows Windows arguments through
# the active ANSI code page. The actual tools must preserve these exact strings.
set(unicode_root "${TEST_ROOT}/Unicode 项目 🚀")
set(unicode_identity "publication-身份-🚀")
set(unicode_target "windows-x64-vulkan")
file(MAKE_DIRECTORY "${unicode_root}")
set(unicode_source "${unicode_root}/契约-🧪.json")
execute_process(COMMAND "${EXPORTER}" --output "${unicode_source}" --build-id "${unicode_identity}"
    --target "${unicode_target}" --config Debug
    RESULT_VARIABLE unicode_export_result OUTPUT_VARIABLE unicode_export_output ERROR_VARIABLE unicode_export_error TIMEOUT 60)
file(WRITE "${TEST_ROOT}/unicode-export.log" "exit=${unicode_export_result}\n${unicode_export_output}\n${unicode_export_error}")

# Independently feed the publication tool a valid factory contract with the same
# requested identity, even if the exporter's separate UTF8 oracle just failed.
file(READ "${TEST_ROOT}/BaselineGuard/source-metadata" unicode_metadata)
string(JSON unicode_metadata SET "${unicode_metadata}" build_id "\"${unicode_identity}\"")
string(JSON unicode_metadata SET "${unicode_metadata}" target "\"${unicode_target}\"")
file(WRITE "${TEST_ROOT}/unicode-input.metadata.json" "${unicode_metadata}\n")
set(unicode_binary "${unicode_root}/模块-🚀${module_suffix}")
file(COPY_FILE "${TEST_ROOT}/BaselineGuard/binary" "${unicode_binary}")
set(unicode_manifest "${unicode_root}/清单-🚀.json")
execute_process(COMMAND "${METADATA_TOOL}" --module "${unicode_binary}"
    --metadata "${TEST_ROOT}/unicode-input.metadata.json" --output "${unicode_root}/元数据-🧪.json"
    --manifest "${unicode_manifest}" --build-id "${unicode_identity}" --target "${unicode_target}" --config Debug
    RESULT_VARIABLE unicode_publish_result OUTPUT_VARIABLE unicode_publish_output ERROR_VARIABLE unicode_publish_error TIMEOUT 60)
file(WRITE "${TEST_ROOT}/unicode-publish.log" "exit=${unicode_publish_result}\n${unicode_publish_output}\n${unicode_publish_error}")
execute_process(COMMAND "${VERIFIER}" --verify "${unicode_manifest}" "${unicode_identity}" "${unicode_target}" 1
    RESULT_VARIABLE unicode_verify_result OUTPUT_VARIABLE unicode_verify_output ERROR_VARIABLE unicode_verify_error TIMEOUT 60)
file(WRITE "${TEST_ROOT}/unicode-verify.log" "exit=${unicode_verify_result}\n${unicode_verify_output}\n${unicode_verify_error}")
set(unicode_source_correct FALSE)
if("${unicode_export_result}" STREQUAL "0" AND EXISTS "${unicode_source}")
    file(READ "${unicode_source}" unicode_exported)
    string(JSON actual_unicode_build GET "${unicode_exported}" build_id)
    string(JSON actual_unicode_target GET "${unicode_exported}" target)
    if(actual_unicode_build STREQUAL unicode_identity AND actual_unicode_target STREQUAL unicode_target)
        set(unicode_source_correct TRUE)
    endif()
endif()
if(NOT unicode_source_correct OR NOT "${unicode_publish_result}" STREQUAL "0" OR
   NOT "${unicode_verify_result}" STREQUAL "0")
    math(EXPR failures "${failures}+1")
    message(STATUS "NON_ACP_UNICODE_PUBLICATION passed=0 export=${unicode_export_result} exact-source=${unicode_source_correct} publish=${unicode_publish_result} verify=${unicode_verify_result}")
else()
    message(STATUS "NON_ACP_UNICODE_PUBLICATION passed=1")
endif()

message(STATUS "MODULE_PUBLICATION_CASES_COMPLETE cases=3 failures=${failures}")
if(NOT failures EQUAL 0)
    message(FATAL_ERROR "Module publication business oracles failed; see the independent case logs")
endif()
