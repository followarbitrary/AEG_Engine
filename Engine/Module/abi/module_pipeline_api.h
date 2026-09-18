#pragma once
#include "module_api.h"
typedef struct AegAbiPipelineApi {
    uint32_t struct_size;
    uint32_t version;
    AegAbiResult(AEG_ABI_CALL* register_type)(AegAbiContext, AegAbiString, uint64_t);
} AegAbiPipelineApi;
typedef AegAbiResult(AEG_ABI_CALL* AegAbiDeclarationOperation)(AegAbiContext, AegAbiBytes, AegAbiContext*);
typedef struct AegAbiRegistrationApi {
    uint32_t struct_size;
    uint32_t version;
    AegAbiResult(AEG_ABI_CALL* register_phase)(AegAbiContext, AegAbiBytes);
    AegAbiResult(AEG_ABI_CALL* register_type)(AegAbiContext, AegAbiBytes);
    AegAbiResult(AEG_ABI_CALL* register_pipeline)(AegAbiContext, AegAbiBytes);
    AegAbiResult(AEG_ABI_CALL* register_feature)(AegAbiContext, AegAbiBytes);
    AegAbiResult(AEG_ABI_CALL* register_pass)(AegAbiContext, AegAbiBytes);
    AegAbiResult(AEG_ABI_CALL* bind_feature)(AegAbiContext, AegAbiBytes);
} AegAbiRegistrationApi;
typedef struct AegAbiDeclarationApi {
    uint32_t struct_size;
    uint32_t version;
    AegAbiDeclarationOperation add_stage;
    AegAbiDeclarationOperation add_task;
    AegAbiDeclarationOperation declare_artifact;
    AegAbiDeclarationOperation add_feature_point;
    AegAbiDeclarationOperation bind_feature;
    AegAbiDeclarationOperation bind_task;
    AegAbiDeclarationOperation reads;
    AegAbiDeclarationOperation writes;
    AegAbiDeclarationOperation reads_writes;
    AegAbiDeclarationOperation publish_snapshot;
    AegAbiDeclarationOperation runs_before;
    AegAbiDeclarationOperation runs_after;
    AegAbiDeclarationOperation expose_artifact;
} AegAbiDeclarationApi;
typedef struct AegAbiTaskApi {
    uint32_t struct_size;
    uint32_t version;
    AegAbiResult(AEG_ABI_CALL* execute)(AegAbiContext, AegAbiContext);
    AegAbiResult(AEG_ABI_CALL* is_cancelled)(AegAbiContext, uint32_t*);
    AegAbiResult(AEG_ABI_CALL* complete)(AegAbiContext, uint32_t);
} AegAbiTaskApi;
