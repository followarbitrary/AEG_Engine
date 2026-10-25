/**
 * @file win32_system_fonts.cpp
 * @brief DirectWrite-backed system font enumeration for Application services.
 */
#include "application_platform.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aeg::application {
namespace {
using Microsoft::WRL::ComPtr;

[[nodiscard]] foundation::Result backend_error(const char* message) {
    return foundation::Result::failure(foundation::ResultCode::kBackendError, message);
}

[[nodiscard]] std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty() || value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int length = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, result.data(), required, nullptr, nullptr) !=
        required) {
        return {};
    }
    return result;
}

[[nodiscard]] foundation::ResultValue<std::string> family_name_for(IDWriteFontFamily* family) {
    ComPtr<IDWriteLocalizedStrings> names;
    if (family == nullptr || FAILED(family->GetFamilyNames(&names)) || names == nullptr) {
        return {foundation::invalid_argument("System font family names are unavailable"), {}};
    }

    UINT32 locale_index = 0;
    BOOL locale_exists = FALSE;
    if (FAILED(names->FindLocaleName(L"en-us", &locale_index, &locale_exists)) || !locale_exists) {
        locale_index = 0;
    }
    UINT32 length = 0;
    if (FAILED(names->GetStringLength(locale_index, &length))) {
        return {foundation::invalid_argument("System font family name length is unavailable"), {}};
    }
    std::wstring wide_name(static_cast<std::size_t>(length) + 1u, L'\0');
    if (FAILED(names->GetString(locale_index, wide_name.data(), length + 1u))) {
        return {foundation::invalid_argument("System font family name is unavailable"), {}};
    }
    wide_name.resize(length);
    std::string utf8_name = wide_to_utf8(wide_name);
    if (utf8_name.empty()) {
        return {foundation::invalid_argument("System font family name is not valid UTF-8"), {}};
    }
    return {foundation::Result::success(), std::move(utf8_name)};
}

[[nodiscard]] const IDWriteFont* choose_font(IDWriteFontFamily* family, ComPtr<IDWriteFont>& selected_font) {
    if (family == nullptr) {
        return nullptr;
    }
    const UINT32 font_count = family->GetFontCount();
    for (UINT32 index = 0; index < font_count; ++index) {
        ComPtr<IDWriteFont> candidate;
        if (FAILED(family->GetFont(index, &candidate)) || candidate == nullptr) {
            continue;
        }
        if (selected_font == nullptr) {
            selected_font = candidate;
        }
        if (candidate->GetWeight() == DWRITE_FONT_WEIGHT_NORMAL && candidate->GetStyle() == DWRITE_FONT_STYLE_NORMAL &&
            candidate->GetStretch() == DWRITE_FONT_STRETCH_NORMAL && candidate->GetSimulations() == DWRITE_FONT_SIMULATIONS_NONE) {
            selected_font = candidate;
            break;
        }
    }
    return selected_font.Get();
}

[[nodiscard]] bool has_glyphs(IDWriteFontFace* face, const std::array<UINT32, 3>& codepoints) noexcept {
    std::array<UINT16, 3> glyphs{};
    if (face == nullptr || FAILED(face->GetGlyphIndices(codepoints.data(), static_cast<UINT32>(codepoints.size()), glyphs.data()))) {
        return false;
    }
    return std::all_of(glyphs.begin(), glyphs.end(), [](UINT16 glyph) { return glyph != 0; });
}

[[nodiscard]] foundation::ResultValue<std::string> font_file_path(IDWriteFontFace* face) {
    if (face == nullptr) {
        return {foundation::invalid_argument("System font face is null"), {}};
    }

    UINT32 file_count = 0;
    const HRESULT count_result = face->GetFiles(&file_count, nullptr);
    if (FAILED(count_result) && count_result != E_NOT_SUFFICIENT_BUFFER) {
        return {foundation::unsupported("System font face files could not be enumerated"), {}};
    }
    if (file_count == 0) {
        return {foundation::unsupported("System font face has no readable file"), {}};
    }

    std::vector<IDWriteFontFile*> raw_files(file_count, nullptr);
    if (FAILED(face->GetFiles(&file_count, raw_files.data()))) {
        return {foundation::unsupported("System font face files could not be enumerated"), {}};
    }
    ComPtr<IDWriteFontFile> file;
    file.Attach(raw_files.front());
    for (std::size_t index = 1; index < raw_files.size(); ++index) {
        if (raw_files[index] != nullptr) {
            raw_files[index]->Release();
        }
    }
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE file_type{};
    DWRITE_FONT_FACE_TYPE face_type{};
    UINT32 face_count = 0;
    if (file == nullptr || FAILED(file->Analyze(&supported, &file_type, &face_type, &face_count)) || !supported ||
        face_count == 0 || (face_type != DWRITE_FONT_FACE_TYPE_TRUETYPE &&
                            face_type != DWRITE_FONT_FACE_TYPE_OPENTYPE_COLLECTION)) {
        return {foundation::unsupported("System font file type is unsupported"), {}};
    }

    const void* reference_key = nullptr;
    UINT32 reference_key_size = 0;
    ComPtr<IDWriteFontFileLoader> loader;
    if (file == nullptr || FAILED(file->GetReferenceKey(&reference_key, &reference_key_size)) ||
        FAILED(file->GetLoader(&loader)) || loader == nullptr) {
        return {foundation::unsupported("System font file loader is unavailable"), {}};
    }

    ComPtr<IDWriteLocalFontFileLoader> local_loader;
    if (FAILED(loader.As(&local_loader)) || local_loader == nullptr) {
        return {foundation::unsupported("System font file path loader is unavailable"), {}};
    }
    UINT32 path_length = 0;
    if (FAILED(local_loader->GetFilePathLengthFromKey(reference_key, reference_key_size, &path_length))) {
        return {foundation::unsupported("System font file path length is unavailable"), {}};
    }
    std::wstring wide_path(static_cast<std::size_t>(path_length) + 1u, L'\0');
    if (FAILED(local_loader->GetFilePathFromKey(reference_key, reference_key_size, wide_path.data(), path_length + 1u))) {
        return {foundation::unsupported("System font file path is unavailable"), {}};
    }
    wide_path.resize(path_length);
    std::string path = wide_to_utf8(wide_path);
    if (path.empty()) {
        return {foundation::invalid_argument("System font file path is not valid UTF-8"), {}};
    }
    return {foundation::Result::success(), std::move(path)};
}

[[nodiscard]] foundation::ResultValue<SystemFontRecord> make_record(IDWriteFontFamily* family) {
    const foundation::ResultValue<std::string> name_result = family_name_for(family);
    if (!name_result) {
        return {name_result.result, {}};
    }

    ComPtr<IDWriteFont> selected_font;
    if (choose_font(family, selected_font) == nullptr) {
        return {foundation::unsupported("System font family has no usable face"), {}};
    }
    ComPtr<IDWriteFontFace> face;
    if (FAILED(selected_font->CreateFontFace(&face)) || face == nullptr) {
        return {foundation::unsupported("System font face could not be created"), {}};
    }
    const foundation::ResultValue<std::string> path_result = font_file_path(face.Get());
    if (!path_result) {
        return {path_result.result, {}};
    }

    SystemFontRecord record{};
    record.family_name = name_result.value;
    record.file_path = path_result.value;
    record.face_index = face->GetIndex();
    record.supports_chinese = has_glyphs(face.Get(), {0x4E2D, 0x6587, 0x7B80});
    record.supports_ligatures = has_glyphs(face.Get(), {0xFB00, 0xFB01, 0xFB02});
    return {foundation::Result::success(), std::move(record)};
}
}  // namespace

foundation::ResultValue<std::vector<SystemFontRecord>> enumerate_system_fonts() {
    try {
        ComPtr<IDWriteFactory> factory;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(factory.GetAddressOf()))) || factory == nullptr) {
            return {backend_error("DirectWrite factory creation failed"), {}};
        }
        ComPtr<IDWriteFontCollection> collection;
        if (FAILED(factory->GetSystemFontCollection(&collection, TRUE)) || collection == nullptr) {
            return {backend_error("DirectWrite system font collection is unavailable"), {}};
        }

        std::vector<SystemFontRecord> records;
        records.reserve(collection->GetFontFamilyCount());
        for (UINT32 index = 0; index < collection->GetFontFamilyCount(); ++index) {
            ComPtr<IDWriteFontFamily> family;
            if (FAILED(collection->GetFontFamily(index, &family)) || family == nullptr) {
                continue;
            }
            const foundation::ResultValue<SystemFontRecord> record_result = make_record(family.Get());
            if (!record_result) {
                continue;
            }
            records.push_back(record_result.value);
        }
        if (records.empty()) {
            return {foundation::unsupported("No usable system fonts were found"), {}};
        }
        std::sort(records.begin(), records.end(), [](const SystemFontRecord& left, const SystemFontRecord& right) {
            return left.family_name < right.family_name;
        });
        return {foundation::Result::success(), std::move(records)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("System font enumeration allocation failed"), {}};
    } catch (const std::exception& exception) {
        return {foundation::unknown_exception(exception), {}};
    } catch (...) {
        return {foundation::Result::failure(foundation::ResultCode::kUnknown,
                                            "System font enumeration threw an unknown exception"), {}};
    }
}
}  // namespace aeg::application
