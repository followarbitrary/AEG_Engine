#pragma once
#include <stdint.h>
#if defined(_WIN32)
#define AEG_ABI_CALL __cdecl
#define AEG_MODULE_EXPORT __declspec(dllexport)
#else
#define AEG_ABI_CALL
#define AEG_MODULE_EXPORT
#endif
#define AEG_MODULE_ABI_MAJOR 1u
#define AEG_MODULE_ABI_MINOR 0u
typedef struct AegAbiContext {
    uint64_t owner_cookie;
    uint32_t slot;
    uint32_t generation;
    uint32_t kind;
    uint32_t reserved;
} AegAbiContext;
typedef struct AegAbiObject {
    AegAbiContext context;
    uint64_t object_id;
} AegAbiObject;
typedef struct AegAbiString {
    const char* data;
    uint64_t size;
} AegAbiString;
typedef struct AegAbiBytes {
    const uint8_t* data;
    uint64_t size;
} AegAbiBytes;
typedef struct AegAbiResult {
    uint32_t struct_size;
    uint32_t code;
    AegAbiString message;
} AegAbiResult;
typedef struct AegAbiByteSink {
    void* user_data;
    AegAbiResult(AEG_ABI_CALL* write)(void* user_data, AegAbiBytes bytes);
} AegAbiByteSink;
typedef struct AegAbiF64Array {
    const double* data;
    uint64_t size;
} AegAbiF64Array;
typedef enum AegAbiValueKind {
    AEG_VALUE_NULL = 0,
    AEG_VALUE_BOOL = 1,
    AEG_VALUE_I64 = 2,
    AEG_VALUE_U64 = 3,
    AEG_VALUE_F64 = 4,
    AEG_VALUE_STRING = 5,
    AEG_VALUE_F64_ARRAY = 6,
    AEG_VALUE_ASSET_UUID = 7,
} AegAbiValueKind;
typedef struct AegAbiValue {
    uint32_t kind;
    uint32_t reserved;
    union {
        uint32_t boolean;
        int64_t i64;
        uint64_t u64;
        double f64;
        AegAbiString string;
        AegAbiF64Array f64_array;
    } data;
} AegAbiValue;
typedef struct AegAbiModuleApi AegAbiModuleApi;
typedef struct AegAbiMetadataApi AegAbiMetadataApi;
typedef struct AegAbiRegistrationApi AegAbiRegistrationApi;
typedef struct AegAbiDeclarationApi AegAbiDeclarationApi;
typedef struct AegAbiTaskApi AegAbiTaskApi;
typedef struct AegAbiRenderGraphApi AegAbiRenderGraphApi;
typedef struct AegAbiPassSetupApi AegAbiPassSetupApi;
typedef struct AegAbiPassCommandApi AegAbiPassCommandApi;
typedef struct AegAbiReflectionApi AegAbiReflectionApi;
typedef AegAbiResult(AEG_ABI_CALL* AegAbiGetModuleApi)(uint32_t, uint32_t, uint32_t, AegAbiModuleApi*);
struct AegAbiModuleApi {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t reserved;
    uint64_t required_capabilities;
    const AegAbiMetadataApi* metadata;
    const AegAbiRegistrationApi* registration;
    const AegAbiDeclarationApi* declaration;
    const AegAbiTaskApi* task;
    const AegAbiRenderGraphApi* render_graph;
    const AegAbiPassSetupApi* pass_setup;
    const AegAbiPassCommandApi* pass_command;
    const AegAbiReflectionApi* reflection;
};
struct AegAbiMetadataApi {
    uint32_t struct_size;
    uint32_t version;
    AegAbiResult(AEG_ABI_CALL* write_metadata)(AegAbiByteSink sink, AegAbiContext context);
};
#ifdef __cplusplus
extern "C" {
#endif
AEG_MODULE_EXPORT AegAbiResult AEG_ABI_CALL aeg_get_module_api(uint32_t requested_major, uint32_t requested_minor,
                                                               uint32_t output_size, AegAbiModuleApi* output);
#ifdef __cplusplus
}
#endif
enum { AEG_ABI_OK = 0, AEG_ABI_INVALID_ARGUMENT = 1, AEG_ABI_UNSUPPORTED = 2, AEG_ABI_INVALID_STATE = 3 };
enum {
    AEG_CAP_METADATA = 1ull << 0,
    AEG_CAP_PIPELINE_DECLARATION = 1ull << 1,
    AEG_CAP_LOGIC = 1ull << 2,
    AEG_CAP_RENDER = 1ull << 3,
    AEG_CAP_VALUE_REFLECTION = 1ull << 4,
    AEG_CAP_RENDER_PROGRAMS = 1ull << 5
};
