#pragma once
#include <stdint.h>

#include "module_api.h"

#define AEG_FEATURE_MODULE_ABI_MAJOR AEG_MODULE_ABI_MAJOR
#define AEG_FEATURE_EXECUTION_API_VERSION 2u
#define AEG_FEATURE_ABI_MAX_BYTES (16u * 1024u * 1024u)
#define AEG_FEATURE_ABI_MAX_DEFINITIONS 65536u

/* Borrowed views live only for the synchronous C callback. Neither side
   destroys memory allocated by the other; responses use the host byte sink. */
typedef struct AegFeatureInvocation {
    uint32_t struct_size;
    uint32_t version;
    uint64_t invocation_id;
    uint64_t attempt_id;
    uint64_t generation;
    uint64_t input_version;
    uint64_t process_revision;
    uint64_t state_version;
    uint64_t snapshot_version;
    uint16_t schema_major;
    uint16_t schema_minor;
    uint16_t schema_patch;
    uint16_t reserved;
    AegAbiBytes state_payload;
    AegAbiString current_state;
    AegAbiBytes input;
    AegAbiBytes parameters;
} AegFeatureInvocation;

enum { AEG_FEATURE_HAS_EXECUTE = 1u, AEG_FEATURE_HAS_STATE_VALIDATOR = 2u };
typedef struct AegFeatureCallbackInfo {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t definition_id;
} AegFeatureCallbackInfo;

struct AegFeatureExecutionApi {
    uint32_t struct_size;
    uint32_t version;
    void* user_data;
    AegAbiContext context;
    uint32_t feature_count;
    uint32_t reserved;
    /* Existing ModuleMetadata encoding of the actual DLL-local factory. */
    AegAbiResult(AEG_ABI_CALL* write_contracts)(void*, AegAbiContext, AegAbiByteSink);
    AegAbiResult(AEG_ABI_CALL* describe_feature)(void*, AegAbiContext, uint32_t, AegFeatureCallbackInfo*);
    /* A bounded, owning AFR1 version-2 result; author failures are carried inside it. */
    AegAbiResult(AEG_ABI_CALL* execute)(void*, AegAbiContext, uint64_t, const AegFeatureInvocation*, AegAbiByteSink);
    AegAbiResult(AEG_ABI_CALL* validate_state)(void*, AegAbiContext, uint64_t, AegAbiBytes, AegAbiByteSink);
};
