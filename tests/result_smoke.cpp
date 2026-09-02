#include <string_view>

#include <result.hpp>

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

    const ResultValue<int> value{success, 42};
    if (!value || value.value != 42) {
        return 1;
    }

    if (std::string_view{aeg::foundation::result_code_name(ResultCode::kOutOfDate)} != "OutOfDate") {
        return 1;
    }

    return 0;
}
