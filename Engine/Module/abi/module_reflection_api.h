#pragma once
#include "module_api.h"
typedef struct AegAbiReflectionApi {
    uint32_t struct_size;
    uint32_t version;
    AegAbiResult(AEG_ABI_CALL* read_value)(AegAbiObject, uint64_t, AegAbiBytes*, AegAbiContext);
    AegAbiResult(AEG_ABI_CALL* write_value)(AegAbiObject, uint64_t, const AegAbiValue*);
    AegAbiResult(AEG_ABI_CALL* serialize_object)(AegAbiObject, AegAbiByteSink);
    AegAbiResult(AEG_ABI_CALL* deserialize_object)(AegAbiObject, AegAbiBytes);
    AegAbiResult(AEG_ABI_CALL* destroy_object)(AegAbiObject);
} AegAbiReflectionApi;
