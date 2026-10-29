#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <vector>

#include "custom_cursor.h"

namespace qpointer {
namespace {

class GdiplusLifetime {
public:
    GdiplusLifetime() {
        Gdiplus::GdiplusStartupInput input;
        status = Gdiplus::GdiplusStartup(&token, &input, nullptr);
    }
    ~GdiplusLifetime() {
        if (status == Gdiplus::Ok) Gdiplus::GdiplusShutdown(token);
    }
    GdiplusLifetime(const GdiplusLifetime&) = delete;
    GdiplusLifetime& operator=(const GdiplusLifetime&) = delete;

    bool Ready() const noexcept { return status == Gdiplus::Ok; }

private:
    ULONG_PTR token = 0;
    Gdiplus::Status status = Gdiplus::GenericError;
};

GdiplusLifetime& GdiplusRuntime() {
    static GdiplusLifetime runtime;
    return runtime;
}

std::wstring StatusText(const wchar_t* action, Gdiplus::Status status) {
    return std::wstring(action) + L"（GDI+ " + std::to_wstring(static_cast<int>(status)) + L"）";
}

std::wstring Win32ErrorText(const wchar_t* action, DWORD code) {
    return std::wstring(action) + L"（Windows " + std::to_wstring(code) + L"）";
}

bool EnsureParentDirectory(const std::wstring& path, std::wstring& error) {
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (!ec) return true;
    error = L"无法创建自定义光标目录。";
    return false;
}

std::unique_ptr<Gdiplus::Bitmap> DecodeScaledBitmap(const std::wstring& path,
    std::wstring& error) {
    error.clear();
    if (!GdiplusRuntime().Ready()) {
        error = L"无法初始化图片解码组件。";
        return nullptr;
    }
    Gdiplus::Bitmap source(path.c_str(), FALSE);
    const Gdiplus::Status sourceStatus = source.GetLastStatus();
    if (sourceStatus != Gdiplus::Ok) {
        error = StatusText(L"无法读取所选图片", sourceStatus);
        return nullptr;
    }
    const UINT sourceWidth = source.GetWidth();
    const UINT sourceHeight = source.GetHeight();
    if (!sourceWidth || !sourceHeight) {
        error = L"所选图片没有可用像素。";
        return nullptr;
    }

    const UINT longSide = std::max(sourceWidth, sourceHeight);
    const double scale = longSide > static_cast<UINT>(kMaxCustomCursorLongSide)
        ? static_cast<double>(kMaxCustomCursorLongSide) / static_cast<double>(longSide)
        : 1.0;
    const UINT width = std::max<UINT>(1, static_cast<UINT>(
        std::lround(static_cast<double>(sourceWidth) * scale)));
    const UINT height = std::max<UINT>(1, static_cast<UINT>(
        std::lround(static_cast<double>(sourceHeight) * scale)));
    auto target = std::make_unique<Gdiplus::Bitmap>(
        static_cast<INT>(width), static_cast<INT>(height), PixelFormat32bppPARGB);
    const Gdiplus::Status targetStatus = target->GetLastStatus();
    if (targetStatus != Gdiplus::Ok) {
        error = StatusText(L"无法创建自定义光标图像", targetStatus);
        return nullptr;
    }

    Gdiplus::Graphics graphics(target.get());
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
    Gdiplus::ImageAttributes attributes;
    attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    const Gdiplus::Rect destination(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    const Gdiplus::Status drawStatus = graphics.DrawImage(&source, destination, 0, 0,
        static_cast<INT>(sourceWidth), static_cast<INT>(sourceHeight),
        Gdiplus::UnitPixel, &attributes);
    if (drawStatus != Gdiplus::Ok) {
        error = StatusText(L"无法转换所选图片", drawStatus);
        return nullptr;
    }
    return target;
}

std::shared_ptr<const CustomCursorImage> CopyPremultipliedPixels(Gdiplus::Bitmap& bitmap,
    std::wstring& error) {
    const UINT sourceWidth = bitmap.GetWidth();
    const UINT sourceHeight = bitmap.GetHeight();
    if (!sourceWidth || !sourceHeight ||
        sourceWidth > static_cast<UINT>(std::numeric_limits<int>::max()) ||
        sourceHeight > static_cast<UINT>(std::numeric_limits<int>::max())) {
        error = L"自定义光标图片尺寸无效。";
        return nullptr;
    }
    const size_t stride = static_cast<size_t>(sourceWidth) * 4;
    if (stride > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        static_cast<size_t>(sourceHeight) > std::numeric_limits<size_t>::max() / stride) {
        error = L"自定义光标图片过大。";
        return nullptr;
    }

    Gdiplus::Rect area(0, 0, static_cast<INT>(sourceWidth), static_cast<INT>(sourceHeight));
    Gdiplus::BitmapData data{};
    const Gdiplus::Status lockStatus = bitmap.LockBits(&area, Gdiplus::ImageLockModeRead,
        PixelFormat32bppPARGB, &data);
    if (lockStatus != Gdiplus::Ok || !data.Scan0 || data.Stride == 0) {
        error = StatusText(L"无法读取自定义光标像素", lockStatus);
        return nullptr;
    }

    auto image = std::make_shared<CustomCursorImage>();
    image->width = static_cast<int>(sourceWidth);
    image->height = static_cast<int>(sourceHeight);
    image->stride = static_cast<int>(stride);
    image->premultipliedBgra.resize(stride * static_cast<size_t>(sourceHeight));
    const auto* first = static_cast<const std::uint8_t*>(data.Scan0);
    const ptrdiff_t sourceStride = static_cast<ptrdiff_t>(data.Stride);
    for (UINT y = 0; y < sourceHeight; ++y) {
        // GDI+ 的 Scan0 是逻辑首行；负 stride 时后续逻辑行位于更低地址。
        const auto* sourceRow = first + static_cast<ptrdiff_t>(y) * sourceStride;
        std::memcpy(image->premultipliedBgra.data() + static_cast<size_t>(y) * stride,
            sourceRow, stride);
    }
    bitmap.UnlockBits(&data);
    return image;
}

bool FindPngEncoder(CLSID& encoder) {
    UINT count = 0;
    UINT bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || !count || !bytes) return false;
    std::vector<std::uint8_t> storage(bytes);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) return false;
    for (UINT index = 0; index < count; ++index) {
        if (encoders[index].MimeType && wcscmp(encoders[index].MimeType, L"image/png") == 0) {
            encoder = encoders[index].Clsid;
            return true;
        }
    }
    return false;
}

double ReadHotspotValue(const std::wstring& path, const wchar_t* key) {
    wchar_t text[64]{};
    GetPrivateProfileStringW(L"cursor", key, L"0.5", text,
        static_cast<DWORD>(std::size(text)), path.c_str());
    wchar_t* end = nullptr;
    const double value = wcstod(text, &end);
    if (end == text || *end != L'\0' || !std::isfinite(value)) return 0.5;
    return std::clamp(value, 0.0, 1.0);
}

bool AtomicWriteUtf16(const std::wstring& destination, const std::wstring& contents,
    std::wstring& error) {
    if (!EnsureParentDirectory(destination, error)) return false;
    const std::wstring temporary = destination + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = Win32ErrorText(L"无法创建光标热点临时文件", GetLastError());
        return false;
    }
    const wchar_t bom = static_cast<wchar_t>(0xfeff);
    DWORD written = 0;
    bool ok = WriteFile(file, &bom, sizeof(bom), &written, nullptr) != FALSE &&
        written == sizeof(bom);
    const size_t byteCount = contents.size() * sizeof(wchar_t);
    if (ok && byteCount > static_cast<size_t>(std::numeric_limits<DWORD>::max())) ok = false;
    if (ok && byteCount) {
        const DWORD expected = static_cast<DWORD>(byteCount);
        ok = WriteFile(file, contents.data(), expected, &written, nullptr) != FALSE &&
            written == expected;
    }
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    const DWORD writeError = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (ok) {
        ok = MoveFileExW(temporary.c_str(), destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!ok) {
        const DWORD code = writeError != ERROR_SUCCESS ? writeError : GetLastError();
        DeleteFileW(temporary.c_str());
        error = Win32ErrorText(L"无法保存光标热点", code);
    }
    return ok;
}

}  // namespace

std::shared_ptr<const CustomCursorImage> LoadCustomCursorImage(
    const std::wstring& path, std::wstring& error) {
    auto bitmap = DecodeScaledBitmap(path, error);
    return bitmap ? CopyPremultipliedPixels(*bitmap, error) : nullptr;
}

bool ImportCustomCursorImage(const std::wstring& source, const std::wstring& destination,
    std::wstring& error) {
    auto bitmap = DecodeScaledBitmap(source, error);
    if (!bitmap || !EnsureParentDirectory(destination, error)) return false;
    CLSID pngEncoder{};
    if (!FindPngEncoder(pngEncoder)) {
        error = L"系统没有可用的 PNG 编码器。";
        return false;
    }
    const std::wstring temporary = destination + L".tmp";
    DeleteFileW(temporary.c_str());
    const Gdiplus::Status saveStatus = bitmap->Save(temporary.c_str(), &pngEncoder, nullptr);
    if (saveStatus != Gdiplus::Ok) {
        DeleteFileW(temporary.c_str());
        error = StatusText(L"无法保存自定义光标图片", saveStatus);
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        DeleteFileW(temporary.c_str());
        error = Win32ErrorText(L"无法替换自定义光标图片", code);
        return false;
    }
    error.clear();
    return true;
}

CustomCursorHotspot LoadCustomCursorHotspot(const std::wstring& metadataPath) {
    return {ReadHotspotValue(metadataPath, L"hotspotX"),
        ReadHotspotValue(metadataPath, L"hotspotY")};
}

bool SaveCustomCursorHotspot(const std::wstring& metadataPath, CustomCursorHotspot hotspot,
    std::wstring& error) {
    hotspot.x = std::clamp(std::isfinite(hotspot.x) ? hotspot.x : 0.5, 0.0, 1.0);
    hotspot.y = std::clamp(std::isfinite(hotspot.y) ? hotspot.y : 0.5, 0.0, 1.0);
    std::wostringstream contents;
    contents.imbue(std::locale::classic());
    contents << L"[cursor]\r\n" << std::setprecision(std::numeric_limits<double>::max_digits10)
        << L"hotspotX=" << hotspot.x << L"\r\n"
        << L"hotspotY=" << hotspot.y << L"\r\n";
    error.clear();
    return AtomicWriteUtf16(metadataPath, contents.str(), error);
}

bool SaveCustomCursorHotspot(const std::wstring& metadataPath, double x, double y,
    std::wstring& error) {
    return SaveCustomCursorHotspot(metadataPath, CustomCursorHotspot{x, y}, error);
}

bool SaveCustomCursorFiles(const std::wstring& sourceImage,
    const std::wstring& destinationImage, const std::wstring& metadataPath,
    CustomCursorHotspot hotspot, std::wstring& error) {
    error.clear();
    if (CompareStringOrdinal(sourceImage.c_str(), -1, destinationImage.c_str(), -1,
            TRUE) == CSTR_EQUAL) {
        return SaveCustomCursorHotspot(metadataPath, hotspot, error);
    }

    const std::wstring stagedImage = destinationImage + L".pending";
    const std::wstring stagedMetadata = metadataPath + L".pending";
    const std::wstring imageBackup = destinationImage + L".rollback";
    const std::wstring metadataBackup = metadataPath + L".rollback";
    DeleteFileW(stagedImage.c_str());
    DeleteFileW(stagedMetadata.c_str());
    DeleteFileW(imageBackup.c_str());
    DeleteFileW(metadataBackup.c_str());

    if (!ImportCustomCursorImage(sourceImage, stagedImage, error) ||
            !SaveCustomCursorHotspot(stagedMetadata, hotspot, error)) {
        DeleteFileW(stagedImage.c_str());
        DeleteFileW(stagedMetadata.c_str());
        return false;
    }

    const auto exists = [](const std::wstring& path) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    };
    const bool hadImage = exists(destinationImage);
    const bool hadMetadata = exists(metadataPath);
    if ((hadImage && !CopyFileW(destinationImage.c_str(), imageBackup.c_str(), FALSE)) ||
            (hadMetadata && !CopyFileW(metadataPath.c_str(), metadataBackup.c_str(), FALSE))) {
        const DWORD code = GetLastError();
        DeleteFileW(stagedImage.c_str());
        DeleteFileW(stagedMetadata.c_str());
        DeleteFileW(imageBackup.c_str());
        DeleteFileW(metadataBackup.c_str());
        error = Win32ErrorText(L"无法备份现有自定义光标", code);
        return false;
    }

    if (!MoveFileExW(stagedImage.c_str(), destinationImage.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        DeleteFileW(stagedImage.c_str());
        DeleteFileW(stagedMetadata.c_str());
        DeleteFileW(imageBackup.c_str());
        DeleteFileW(metadataBackup.c_str());
        error = Win32ErrorText(L"无法提交自定义光标图片", code);
        return false;
    }

    if (!MoveFileExW(stagedMetadata.c_str(), metadataPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        const bool restored = hadImage
            ? MoveFileExW(imageBackup.c_str(), destinationImage.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE
            : DeleteFileW(destinationImage.c_str()) != FALSE ||
                GetLastError() == ERROR_FILE_NOT_FOUND;
        DeleteFileW(stagedMetadata.c_str());
        DeleteFileW(metadataBackup.c_str());
        if (restored) DeleteFileW(imageBackup.c_str());
        error = Win32ErrorText(L"无法提交光标热点", code);
        if (!restored) error += L"；旧图片自动恢复失败，备份保留在 .rollback 文件。";
        return false;
    }

    DeleteFileW(imageBackup.c_str());
    DeleteFileW(metadataBackup.c_str());
    error.clear();
    return true;
}

}  // namespace qpointer
