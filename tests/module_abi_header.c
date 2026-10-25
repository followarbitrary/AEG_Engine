#include <stddef.h>

#include "../Engine/Module/abi/module_api.h"
#include "../Engine/Module/abi/feature_module_api.h"
#include "../Engine/Module/abi/module_reflection_api.h"
typedef char aeg_context_layout[(sizeof(AegAbiContext) == 24) ? 1 : -1];
typedef char aeg_size_prefix[(offsetof(AegAbiModuleApi, struct_size) == 0) ? 1 : -1];
typedef char aeg_feature_info_layout[(sizeof(AegFeatureCallbackInfo) == 16) ? 1 : -1];
typedef char aeg_feature_token_width[(sizeof(((AegFeatureInvocation*)0)->invocation_id) == 8) ? 1 : -1];
typedef char aeg_feature_view_prefix[(offsetof(AegFeatureInvocation, struct_size) == 0) ? 1 : -1];
int main(void) {
    return AEG_MODULE_ABI_MAJOR == 2u ? 0 : 1;
}
