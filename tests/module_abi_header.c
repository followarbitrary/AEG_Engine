#include <stddef.h>

#include "../Engine/Module/abi/module_api.h"
#include "../Engine/Module/abi/module_pipeline_api.h"
#include "../Engine/Module/abi/module_reflection_api.h"
#include "../Engine/Module/abi/module_render_api.h"
typedef char aeg_context_layout[(sizeof(AegAbiContext) == 24) ? 1 : -1];
typedef char aeg_size_prefix[(offsetof(AegAbiModuleApi, struct_size) == 0) ? 1 : -1];
int main(void) {
    return AEG_MODULE_ABI_MAJOR == 1u ? 0 : 1;
}
