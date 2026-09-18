#pragma once
#include "module_api.h"
typedef struct AegAbiGraphHandle {
    uint64_t graph_id;
    uint32_t index;
    uint32_t generation;
} AegAbiGraphHandle;
typedef struct AegAbiRenderApi {
    uint32_t struct_size;
    uint32_t version;
    AegAbiResult(AEG_ABI_CALL* add_pass)(AegAbiContext, AegAbiString, AegAbiGraphHandle*);
} AegAbiRenderApi;
typedef struct AegAbiRenderGraphApi {
    uint32_t struct_size;
    uint32_t version;
    AegAbiResult(AEG_ABI_CALL* create_resource)(AegAbiContext, AegAbiBytes, AegAbiGraphHandle*);
    AegAbiResult(AEG_ABI_CALL* find_resource)(AegAbiContext, AegAbiString, AegAbiGraphHandle*);
    AegAbiResult(AEG_ABI_CALL* bind_resource)(AegAbiContext, AegAbiGraphHandle);
    AegAbiResult(AEG_ABI_CALL* export_resource)(AegAbiContext, AegAbiGraphHandle);
    AegAbiResult(AEG_ABI_CALL* add_pass)(AegAbiContext, AegAbiString, AegAbiGraphHandle*);
} AegAbiRenderGraphApi;
typedef struct AegAbiPassSetupApi {
    uint32_t struct_size;
    uint32_t version;
    AegAbiResult(AEG_ABI_CALL* read_resource)(AegAbiContext, AegAbiGraphHandle, AegAbiBytes);
    AegAbiResult(AEG_ABI_CALL* write_resource)(AegAbiContext, AegAbiGraphHandle, AegAbiBytes);
    AegAbiResult(AEG_ABI_CALL* read_write_resource)(AegAbiContext, AegAbiGraphHandle, AegAbiBytes);
    AegAbiResult(AEG_ABI_CALL* declare_render_program)(AegAbiContext, AegAbiBytes);
    AegAbiResult(AEG_ABI_CALL* set_side_effects)(AegAbiContext, uint32_t);
} AegAbiPassSetupApi;
typedef struct AegAbiPassCommandApi {
    uint32_t struct_size;
    uint32_t version;
    AegAbiResult(AEG_ABI_CALL* resolve_resource)(AegAbiContext, AegAbiGraphHandle, AegAbiGraphHandle*);
    AegAbiResult(AEG_ABI_CALL* bind_pipeline)(AegAbiContext, AegAbiGraphHandle);
    AegAbiResult(AEG_ABI_CALL* bind_render_program)(AegAbiContext, AegAbiBytes);
    AegAbiResult(AEG_ABI_CALL* draw)(AegAbiContext, AegAbiBytes);
    AegAbiResult(AEG_ABI_CALL* dispatch)(AegAbiContext, AegAbiBytes);
} AegAbiPassCommandApi;
