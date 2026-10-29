#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qpointer {

struct Point { double x = 0, y = 0; };
struct Rect { double x = 0, y = 0, w = 0, h = 0; };
struct Affine { double a = 1, b = 0, c = 0, d = 0, e = 1, f = 0; };

// target 为桌面物理像素；content/crop 均为旋转前完整采集帧的归一化矩形。
struct Geometry {
    Rect target{0, 0, 1920, 1080};
    double frameWidth = 1920, frameHeight = 1080;
    Rect content{0, 0, 1, 1};
    Rect crop{0, 0, 1, 1};
    int rotation = 0;
    bool mirrorX = false, mirrorY = false, stretch = false;
    Affine calibration;
};

bool ValidateGeometry(const Geometry& geometry, std::string& error);
// 返回视频在目标区域内实际占用的矩形；等比显示时已排除播放器生成的黑边。
Rect DisplayRect(const Geometry& geometry);
std::optional<Point> MapPoint(const Geometry& geometry, Point input);
// 反解显示、旋转镜像、裁剪及内容区域，不逆校准 A；结果用于拟合可见触点。
std::optional<Point> UnmapPoint(const Geometry& geometry, Point screen);
// 将完整输入范围映射到用户在画面上框选的矩形，用于采集卡快捷范围校准。
bool FitRangeCalibration(const Geometry& geometry, Rect screenRange,
                         Affine& result, std::string& error);
bool FitAffine(const std::vector<Point>& input, const std::vector<Point>& observed,
               Affine& result, double& rms, double& maxError, std::string& error);

enum class PacketKind { Move, Show, Hide, Heartbeat };
struct Packet {
    PacketKind kind = PacketKind::Move;
    std::optional<double> nx, ny;
    std::optional<bool> show, down;
};
bool ParsePacket(const std::string& json, Packet& packet, std::string& error);

struct PointerState {
    Point position{0.5, 0.5};
    bool hasPosition = false, show = false, down = false;
    std::uint64_t lastPacketMs = 0;
};
// 返回是否接受报文；首个不完整坐标不能建立有效位置。heartbeat 只保活。
bool ApplyPacket(PointerState& state, const Packet& packet, std::uint64_t now);

}  // namespace qpointer
