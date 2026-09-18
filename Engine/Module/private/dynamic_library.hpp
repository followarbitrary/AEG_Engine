#pragma once

#include <filesystem>
#include <string_view>

#include "result.hpp"

namespace aeg::module::detail {
class DynamicLibrary final {
public:
    DynamicLibrary() noexcept = default;
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;
    ~DynamicLibrary();

    [[nodiscard]] static foundation::ResultValue<DynamicLibrary> open(const std::filesystem::path& path);
    [[nodiscard]] static DynamicLibrary adopt(void* native_handle) noexcept;
    [[nodiscard]] foundation::ResultValue<void*> symbol(std::string_view name) const;
    [[nodiscard]] foundation::Result close() noexcept;
    [[nodiscard]] void* detach() noexcept;
    [[nodiscard]] bool valid() const noexcept { return native_handle_ != nullptr; }

private:
    explicit DynamicLibrary(void* native_handle) noexcept : native_handle_(native_handle) {}
    void* native_handle_{nullptr};
};
}  // namespace aeg::module::detail
