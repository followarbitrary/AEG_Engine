#pragma once
#include "../Engine/Module/abi/module_api.h"

/* Test-only instrumentation called from inside the real DLL author callback. */
typedef struct AegDynamicFeatureProbe {
    void* user_data;
    void(AEG_ABI_CALL* factory_entered)(void*);
    void(AEG_ABI_CALL* execute_entered)(void*, uint64_t);
    void(AEG_ABI_CALL* state_validated)(void*, uint64_t);
} AegDynamicFeatureProbe;
typedef void(AEG_ABI_CALL* AegDynamicBindProbe)(AegDynamicFeatureProbe);
