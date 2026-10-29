#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qpointer {

inline constexpr int kMaxCustomCursorLongSide = 512;

// D2D 可直接上传的预乘 BGRA 图像。stride 始终为正，像素按从上到下存放。
struct CustomCursorImage {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<std::uint8_t> premultipliedBgra;
};

struct CustomCursorHotspot {
    double x = 0.5;
    double y = 0.5;
};

// 加载常见图片并转换为预乘 BGRA；超大图片会按比例缩到长边 512 像素。
// GDI+ 生命周期由本模块独立管理，调用方可以在任意普通工作线程调用。
std::shared_ptr<const CustomCursorImage> LoadCustomCursorImage(
    const std::wstring& path, std::wstring& error);

// 将用户选择的图片缩放并重新编码为 PNG，再在同目录原子替换 destination。
bool ImportCustomCursorImage(const std::wstring& source, const std::wstring& destination,
    std::wstring& error);

// 热点是图片内的归一化坐标。文件缺失、损坏或越界时分别回退或夹到 0..1。
CustomCursorHotspot LoadCustomCursorHotspot(const std::wstring& metadataPath);
bool SaveCustomCursorHotspot(const std::wstring& metadataPath, CustomCursorHotspot hotspot,
    std::wstring& error);
bool SaveCustomCursorHotspot(const std::wstring& metadataPath, double x, double y,
    std::wstring& error);

// 图片与热点先分别写入暂存文件，再提交为一组。第二个文件提交失败时会恢复旧图片，
// 避免下次启动出现“新图片配旧热点”。sourceImage 与 destinationImage 相同时只更新热点。
bool SaveCustomCursorFiles(const std::wstring& sourceImage,
    const std::wstring& destinationImage, const std::wstring& metadataPath,
    CustomCursorHotspot hotspot, std::wstring& error);

}  // namespace qpointer
