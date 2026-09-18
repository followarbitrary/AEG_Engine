/** @file asset_identity.cpp */
#include "asset_identity.hpp"

#include <string>

namespace aeg::asset {
namespace {
[[nodiscard]] int hex(char c) noexcept {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
[[nodiscard]] char digit(uint8_t v) noexcept {
    constexpr std::string_view kDigits{"0123456789abcdef"};
    return kDigits[v & 0x0fU];
}
}  // namespace

foundation::ResultValue<Uuid> Uuid::parse(std::string_view text) {
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') {
        return {foundation::invalid_argument("UUID must use canonical 8-4-4-4-12 hexadecimal form"), {}};
    }
    Uuid uuid{};
    for (std::size_t out = 0; out < uuid.bytes.size(); ++out) {
        const std::size_t index = out * 2 + (out >= 4) + (out >= 6) + (out >= 8) + (out >= 10);
        const int high = hex(text[index]);
        const int low = hex(text[index + 1]);
        if (high < 0 || low < 0) {
            return {foundation::invalid_argument("UUID contains a non-hexadecimal digit"), {}};
        }
        uuid.bytes[out] = static_cast<uint8_t>((high << 4) | low);
    }
    return {foundation::Result::success(), uuid};
}

std::string Uuid::to_string() const {
    std::string text;
    text.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            text.push_back('-');
        text.push_back(digit(static_cast<uint8_t>(bytes[index] >> 4U)));
        text.push_back(digit(bytes[index]));
    }
    return text;
}

foundation::ResultValue<AssetId> AssetId::parse(std::string_view text) {
    const auto uuid = Uuid::parse(text);
    return uuid ? foundation::ResultValue<AssetId>{foundation::Result::success(), AssetId{uuid.value}}
                : foundation::ResultValue<AssetId>{uuid.result, {}};
}

std::string AssetId::to_string() const {
    return value.to_string();
}
}  // namespace aeg::asset
