/**
 * @file shader_binary.hpp
 * @brief Non-owning view of a compiled shader binary embedded in the executable.
 */
#pragma once

#include <cstddef>
#include <span>

namespace aeg::shaders {
    /** @brief Non-owning byte range containing one compiled shader stage. */
    struct ShaderBinary {
        const std::byte* data{nullptr};
        std::size_t size{0};

        [[nodiscard]] std::span<const std::byte> as_span() const noexcept {
            return {data, size};
        }
    };
} // namespace aeg::shaders
