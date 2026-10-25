#pragma once

#include "../Engine/Module/abi/module_api.h"

typedef void(AEG_ABI_CALL* AegWorldMotionSetGate)(uint32_t released);
typedef uint32_t(AEG_ABI_CALL* AegWorldMotionGetCallCount)();
