/**
 * @file win32_startup_splash.cpp
 * @brief Win32 startup splash implementation backed by GDI+ image decoding.
 */
// GDI+ headers require the Win32 base types and GDI constants first.
// clang-format off
#include <windows.h>
#include <propidl.h>
#include <gdiplus.h>
// clang-format on

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "application_platform.hpp"

namespace aeg::application {
namespace {
constexpr wchar_t kSplashWindowClassName[] = L"AEG.Engine.Application.StartupSplash";

foundation::Result splash_backend_error(const char* message) {
    return foundation::Result::failure(foundation::ResultCode::kBackendError, message);
}

foundation::ResultValue<std::wstring> to_wide_string(std::string_view value) {
    if (value.empty() || value.find('\0') != std::string_view::npos ||
        value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {foundation::invalid_argument("Startup splash name is invalid"), {}};
    }
    const int source_length = static_cast<int>(value.size());
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), source_length, nullptr, 0);
    if (required <= 0) {
        return {splash_backend_error("Startup splash name conversion failed"), {}};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), source_length, result.data(), required) !=
        required) {
        return {splash_backend_error("Startup splash name conversion failed"), {}};
    }
    return {foundation::Result::success(), std::move(result)};
}

class Win32StartupSplash final : public IStartupSplash {
public:
    ~Win32StartupSplash() override {
        close();
        image_.reset();
        if (gdiplus_token_ != 0) {
            Gdiplus::GdiplusShutdown(gdiplus_token_);
        }
    }

    Win32StartupSplash(const Win32StartupSplash&) = delete;
    Win32StartupSplash& operator=(const Win32StartupSplash&) = delete;

    [[nodiscard]] static foundation::ResultValue<std::unique_ptr<IStartupSplash>> create(
        const StartupSplashDesc& desc) {
        if (desc.image_path.empty()) {
            return {foundation::invalid_argument("Startup splash image path must not be empty"), {}};
        }
        if (!desc.extent.is_valid() || desc.extent.width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
            desc.extent.height > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
            return {foundation::invalid_argument("Startup splash extent is invalid"), {}};
        }
        if (desc.accessible_name.empty()) {
            return {foundation::invalid_argument("Startup splash name must not be empty"), {}};
        }
        std::error_code exists_error;
        const bool image_exists = std::filesystem::is_regular_file(desc.image_path, exists_error);
        if (exists_error || !image_exists) {
            return {foundation::not_found("Startup splash image was not found"), {}};
        }

        try {
            auto splash = std::unique_ptr<Win32StartupSplash>(new Win32StartupSplash());
            Gdiplus::GdiplusStartupInput startup_input{};
            if (Gdiplus::GdiplusStartup(&splash->gdiplus_token_, &startup_input, nullptr) != Gdiplus::Ok) {
                return {splash_backend_error("GDI+ startup failed for the startup splash"), {}};
            }
            splash->image_ = std::make_unique<Gdiplus::Image>(desc.image_path.c_str());
            if (splash->image_->GetLastStatus() != Gdiplus::Ok || splash->image_->GetWidth() == 0 ||
                splash->image_->GetHeight() == 0) {
                return {splash_backend_error("Startup splash image decoding failed"), {}};
            }
            splash->extent_ = desc.extent;

            const HINSTANCE instance = GetModuleHandleW(nullptr);
            if (instance == nullptr) {
                return {splash_backend_error("Application module handle is unavailable"), {}};
            }
            WNDCLASSEXW window_class{};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = &Win32StartupSplash::window_proc;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.lpszClassName = kSplashWindowClassName;
            if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                return {splash_backend_error("Startup splash window class registration failed"), {}};
            }

            const auto title_result = to_wide_string(desc.accessible_name);
            if (!title_result) {
                return {title_result.result, {}};
            }
            RECT work_area{};
            if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0) == FALSE) {
                return {splash_backend_error("Startup splash display work area query failed"), {}};
            }
            const int width = static_cast<int>(desc.extent.width);
            const int height = static_cast<int>(desc.extent.height);
            const int x = work_area.left + std::max((work_area.right - work_area.left - width) / 2, 0L);
            const int y = work_area.top + std::max((work_area.bottom - work_area.top - height) / 2, 0L);
            splash->window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                              kSplashWindowClassName, title_result.value.c_str(), WS_POPUP, x, y, width,
                                              height, nullptr, nullptr, instance, splash.get());
            if (splash->window_ == nullptr) {
                return {splash_backend_error("Startup splash window creation failed"), {}};
            }
            const foundation::Result present_result = splash->present(x, y);
            if (!present_result) {
                splash->close();
                return {present_result, {}};
            }
            ShowWindow(splash->window_, SW_SHOWNOACTIVATE);
            return {foundation::Result::success(), std::unique_ptr<IStartupSplash>(std::move(splash))};
        } catch (const std::bad_alloc&) {
            return {foundation::out_of_memory("Startup splash allocation failed"), {}};
        } catch (const std::exception& exception) {
            return {foundation::unknown_exception(exception), {}};
        }
    }

    void close() noexcept override {
        if (window_ != nullptr) {
            const HWND window = std::exchange(window_, nullptr);
            DestroyWindow(window);
        }
    }

private:
    Win32StartupSplash() = default;

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) noexcept {
        auto* splash = reinterpret_cast<Win32StartupSplash*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
            splash = static_cast<Win32StartupSplash*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(splash));
        }
        if (message == WM_ERASEBKGND) {
            return 1;
        }
        if (message == WM_CLOSE) {
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_NCDESTROY) {
            if (splash != nullptr && splash->window_ == window) {
                splash->window_ = nullptr;
            }
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(window, message, w_param, l_param);
    }

    [[nodiscard]] foundation::Result present(int screen_x, int screen_y) noexcept {
        const int canvas_width = static_cast<int>(extent_.width);
        const int canvas_height = static_cast<int>(extent_.height);
        Gdiplus::Bitmap canvas(canvas_width, canvas_height, PixelFormat32bppPARGB);
        if (canvas.GetLastStatus() != Gdiplus::Ok) {
            return splash_backend_error("Startup splash transparent surface creation failed");
        }
        Gdiplus::Graphics graphics(&canvas);
        if (graphics.GetLastStatus() != Gdiplus::Ok) {
            return splash_backend_error("Startup splash transparent graphics creation failed");
        }
        graphics.Clear(Gdiplus::Color{0, 0, 0, 0});
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

        const float available_width = static_cast<float>(extent_.width) * 0.86f;
        const float available_height = static_cast<float>(extent_.height) * 0.86f;
        const float image_width = static_cast<float>(image_->GetWidth());
        const float image_height = static_cast<float>(image_->GetHeight());
        const float scale = std::min(available_width / image_width, available_height / image_height);
        const float width = image_width * scale;
        const float height = image_height * scale;
        const float image_x = (static_cast<float>(extent_.width) - width) * 0.5f;
        const float image_y = (static_cast<float>(extent_.height) - height) * 0.5f;
        if (graphics.DrawImage(image_.get(), Gdiplus::RectF{image_x, image_y, width, height}) != Gdiplus::Ok) {
            return splash_backend_error("Startup splash image rendering failed");
        }

        HBITMAP bitmap = nullptr;
        if (canvas.GetHBITMAP(Gdiplus::Color{0, 0, 0, 0}, &bitmap) != Gdiplus::Ok || bitmap == nullptr) {
            return splash_backend_error("Startup splash transparent bitmap creation failed");
        }
        HDC screen_context = GetDC(nullptr);
        HDC memory_context = screen_context == nullptr ? nullptr : CreateCompatibleDC(screen_context);
        HGDIOBJ previous_bitmap = memory_context == nullptr ? nullptr : SelectObject(memory_context, bitmap);
        POINT destination{screen_x, screen_y};
        POINT source{0, 0};
        SIZE size{canvas_width, canvas_height};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        const BOOL updated = memory_context != nullptr && previous_bitmap != nullptr &&
                             UpdateLayeredWindow(window_, screen_context, &destination, &size, memory_context, &source,
                                                 0, &blend, ULW_ALPHA);
        if (previous_bitmap != nullptr) {
            SelectObject(memory_context, previous_bitmap);
        }
        if (memory_context != nullptr) {
            DeleteDC(memory_context);
        }
        if (screen_context != nullptr) {
            ReleaseDC(nullptr, screen_context);
        }
        DeleteObject(bitmap);
        if (updated == FALSE) {
            return splash_backend_error("Startup splash transparent window update failed");
        }
        return foundation::Result::success();
    }

    ULONG_PTR gdiplus_token_{0};
    HWND window_{nullptr};
    platform::WindowExtent extent_{};
    std::unique_ptr<Gdiplus::Image> image_{};
};
}  // namespace

foundation::ResultValue<std::unique_ptr<IStartupSplash>> show_startup_splash(const StartupSplashDesc& desc) {
    return Win32StartupSplash::create(desc);
}
}  // namespace aeg::application
