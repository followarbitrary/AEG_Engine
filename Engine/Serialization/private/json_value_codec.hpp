#pragma once

#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "parameter_value.hpp"
#include "result.hpp"

namespace aeg::serialization::detail {
using Json = nlohmann::json;

inline foundation::Result invalid(std::string_view field) {
    return foundation::invalid_argument("JSON field is invalid: " + std::string(field));
}

template <typename Id>
Json id_json(Id id) {
    return std::to_string(id.value());
}

template <typename Id>
foundation::ResultValue<Id> id_value(const Json& value, std::string_view name) {
    if (!value.contains(name) || !value.at(name).is_string())
        return {invalid(name), {}};
    const auto text = value.at(name).get<std::string>();
    uint64_t number{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || number == 0)
        return {invalid(name), {}};
    return {foundation::Result::success(), Id{number}};
}

inline foundation::ResultValue<uint64_t> u64_value(const Json& value, std::string_view name) {
    if (!value.contains(name))
        return {invalid(name), {}};
    if (value.at(name).is_string()) {
        const auto text = value.at(name).get<std::string>();
        uint64_t number{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
            return {foundation::Result::success(), number};
    } else if (value.at(name).is_number_unsigned()) {
        return {foundation::Result::success(), value.at(name).get<uint64_t>()};
    }
    return {invalid(name), {}};
}

template <typename T>
foundation::ResultValue<T> u32_value(const Json& value, std::string_view name) {
    if (!value.contains(name) || !value.at(name).is_number_unsigned())
        return {invalid(name), {}};
    const auto number = value.at(name).get<uint64_t>();
    if (number > std::numeric_limits<T>::max())
        return {invalid(name), {}};
    return {foundation::Result::success(), static_cast<T>(number)};
}

inline foundation::ResultValue<std::string> string_value(const Json& value, std::string_view name) {
    if (!value.contains(name) || !value.at(name).is_string())
        return {invalid(name), {}};
    return {foundation::Result::success(), value.at(name).get<std::string>()};
}

inline foundation::ResultValue<bool> bool_value(const Json& value, std::string_view name) {
    if (!value.contains(name) || !value.at(name).is_boolean())
        return {invalid(name), {}};
    return {foundation::Result::success(), value.at(name).get<bool>()};
}

struct JsonVersion final { uint32_t major{}, minor{}, patch{}; };

inline Json version_json(JsonVersion value) {
    return Json{{"major", value.major}, {"minor", value.minor}, {"patch", value.patch}};
}
inline foundation::ResultValue<JsonVersion> version_value(const Json& value, std::string_view name) {
    if (!value.contains(name) || !value.at(name).is_object())
        return {invalid(name), {}};
    const auto& item = value.at(name);
    JsonVersion result{};
    const auto major = u32_value<uint32_t>(item, "major");
    const auto minor = u32_value<uint32_t>(item, "minor");
    const auto patch = u32_value<uint32_t>(item, "patch");
    if (!major || !minor || !patch)
        return {invalid(name), {}};
    result.major = major.value;
    result.minor = minor.value;
    result.patch = patch.value;
    return {foundation::Result::success(), result};
}

inline Json parameter_json(const foundation::ParameterValue& value) {
    Json result;
    if (const auto* bool_value = std::get_if<bool>(&value))
        result = {{"kind", "bool"}, {"value", *bool_value}};
    else if (const auto* integer_value = std::get_if<int64_t>(&value))
        result = {{"kind", "i64"}, {"value", std::to_string(*integer_value)}};
    else if (const auto* unsigned_value = std::get_if<uint64_t>(&value))
        result = {{"kind", "u64"}, {"value", std::to_string(*unsigned_value)}};
    else if (const auto* float_value = std::get_if<double>(&value)) {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(17) << (*float_value == 0.0 ? 0.0 : *float_value);
        result = {{"kind", "f64"}, {"value", stream.str()}};
    } else if (const auto* string_value = std::get_if<std::string>(&value))
        result = {{"kind", "string"}, {"value", *string_value}};
    else if (const auto* vector_value = std::get_if<std::vector<double>>(&value)) {
        Json array = Json::array();
        for (const double number : *vector_value) {
            std::ostringstream stream;
            stream.imbue(std::locale::classic());
            stream << std::setprecision(17) << (number == 0.0 ? 0.0 : number);
            array.push_back(stream.str());
        }
        result = {{"kind", "vector"}, {"value", std::move(array)}};
    } else
        result = {{"kind", "asset"}, {"value", std::get<foundation::AssetReference>(value).uuid}};
    return result;
}

inline foundation::ResultValue<foundation::ParameterValue> parameter_value(const Json& value) {
    const auto kind = string_value(value, "kind");
    if (!kind || !value.contains("value"))
        return {invalid("parameter"), {}};
    const auto& raw = value.at("value");
    if (kind.value == "bool" && raw.is_boolean())
        return {foundation::Result::success(), raw.get<bool>()};
    if (kind.value == "i64" && raw.is_string()) {
        int64_t number{};
        const auto text = raw.get<std::string>();
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
            return {foundation::Result::success(), number};
    }
    if (kind.value == "u64" && raw.is_string()) {
        uint64_t number{};
        const auto text = raw.get<std::string>();
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
            return {foundation::Result::success(), number};
    }
    if (kind.value == "string" && raw.is_string())
        return {foundation::Result::success(), raw.get<std::string>()};
    if (kind.value == "asset" && raw.is_string())
        return {foundation::Result::success(), foundation::AssetReference{raw.get<std::string>()}};
    if (kind.value == "f64" && raw.is_string()) {
        try {
            const auto text = raw.get<std::string>();
            size_t consumed{};
            const double number = std::stod(text, &consumed);
            if (consumed == text.size() && std::isfinite(number))
                return {foundation::Result::success(), number};
        } catch (...) {}
    }
    if (kind.value == "vector" && raw.is_array()) {
        std::vector<double> result;
        result.reserve(raw.size());
        for (const auto& item : raw) {
            if (!item.is_string())
                return {invalid("vector"), {}};
            try {
                const auto text = item.get<std::string>();
                size_t consumed{};
                const double number = std::stod(text, &consumed);
                if (consumed != text.size() || !std::isfinite(number))
                    return {invalid("vector"), {}};
                result.push_back(number);
            } catch (...) {
                return {invalid("vector"), {}};
            }
        }
        return {foundation::Result::success(), std::move(result)};
    }
    return {invalid("parameter value"), {}};
}

}  // namespace aeg::serialization::detail
