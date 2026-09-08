/**
 * @file result_smoke.cpp
 * @brief Exercises the common Foundation result value types.
 */
#include <result.hpp>
#include <string_view>

int main() {
    using aeg::foundation::Result;
    using aeg::foundation::ResultCode;
    using aeg::foundation::ResultValue;

    const Result success = Result::success();
    if (!success || success.code != ResultCode::kSuccess) {
        return 1;
    }

    const Result failure = Result::failure(ResultCode::kInvalidArgument, "invalid value");
    if (failure || failure.code != ResultCode::kInvalidArgument || failure.message != "invalid value") {
        return 1;
    }

    if (aeg::foundation::invalid_argument("argument").code != ResultCode::kInvalidArgument ||
        aeg::foundation::invalid_state("state").code != ResultCode::kInvalidState ||
        aeg::foundation::unsupported("unsupported").code != ResultCode::kUnsupported ||
        aeg::foundation::not_found("missing").code != ResultCode::kNotFound ||
        aeg::foundation::not_ready("pending").code != ResultCode::kNotReady ||
        aeg::foundation::out_of_memory("allocation").code != ResultCode::kOutOfMemory) {
        return 1;
    }

    const ResultValue<int> value{success, 42};
    if (!value || value.value != 42) {
        return 1;
    }

    if (std::string_view{aeg::foundation::result_code_name(ResultCode::kOutOfDate)} != "OutOfDate") {
        return 1;
    }

    return 0;
}
