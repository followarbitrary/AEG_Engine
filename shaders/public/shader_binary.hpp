#pragma once

#include <cstddef>
#include <span>

namespace aeg::shaders {
    struct ShaderBinary {
        const std::byte* data{nullptr};
        std::size_t size{0};

        [[nodiscard]] std::span<const std::byte> as_span() const noexcept {
            return {data, size};
        }
    };
} // namespace aeg::shaders
