// 覆盖公开报文状态与显示几何；无需网络、采集卡或手机。
#include "pointer_core.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace qp = qpointer;
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool Near(double a, double b) { return std::abs(a - b) < 1e-7; }
int main() {
    try {
        qp::Packet packet; std::string error; qp::PointerState state;
        Check(qp::ParsePacket(R"({"type":"show"})", packet, error), "解析 show");
        qp::ApplyPacket(state, packet, 1);
        Check(!state.show && !state.hasPosition, "没有位置时不能显示");
        Check(qp::ParsePacket(R"({"type":"move","nx":0.25,"ny":0.75,"down":true})", packet, error), "解析 move");
        Check(qp::ApplyPacket(state, packet, 2) && state.show && state.down, "移动建立位置和按下态");
        Check(qp::ParsePacket(R"({"type":"move","nx":0.5})", packet, error), "解析部分更新");
        Check(qp::ApplyPacket(state, packet, 3) && Near(state.position.x, .5) && Near(state.position.y, .75) && state.down, "部分更新保留状态");
        Check(qp::ParsePacket(R"({"type":"hide"})", packet, error), "解析 hide");
        Check(qp::ApplyPacket(state, packet, 4) && !state.show && !state.down, "隐藏清除按下态");
        Check(qp::ParsePacket(R"({"type":"heartbeat"})", packet, error), "解析 heartbeat");
        qp::ApplyPacket(state, packet, 5);
        Check(!state.show && state.lastPacketMs == 5, "保活不恢复可见状态");
        for (const auto* invalid : {R"({"type":"move","nx":2})", R"({"type":"hide","show":1})", R"({"type":"move","nx":0,"nx":1})", R"({"type":"unknown"})", R"({"type":"move","nx":1e999})"})
            Check(!qp::ParsePacket(invalid, packet, error), "非法报文必须拒绝");
        qp::Geometry geometry; geometry.target = {-1000, 50, 1001, 501}; geometry.stretch = true;
        auto point = qp::MapPoint(geometry, {0, 0});
        Check(point && Near(point->x, -1000) && Near(point->y, 50), "负坐标显示器左上角");
        point = qp::MapPoint(geometry, {1, 1});
        Check(point && Near(point->x, 0) && Near(point->y, 550), "像素边界");
        for (int rotation : {0, 90, 180, 270}) {
            geometry.rotation = rotation; geometry.mirrorX = true; geometry.mirrorY = true;
            point = qp::MapPoint(geometry, {.23, .71});
            Check(point.has_value(), "旋转映射有效");
            auto reverse = qp::UnmapPoint(geometry, *point);
            Check(reverse && Near(reverse->x, .23) && Near(reverse->y, .71), "旋转镜像往返");
        }
        geometry = {}; geometry.crop = {.25, .25, .5, .5};
        Check(!qp::MapPoint(geometry, {0, 0}), "裁剪外位置隐藏");
        qp::Affine fit; double rms = 0, maximum = 0;
        Check(qp::FitAffine({{0,0},{1,0},{0,1},{1,1}}, {{.1,.2},{.9,.2},{.1,.8},{.9,.8}}, fit, rms, maximum, error), "校准可拟合");
        Check(Near(fit.a,.8) && Near(fit.c,.1) && Near(fit.e,.6) && Near(fit.f,.2), "校准矩阵");
        Check(!qp::FitAffine({{0,0},{.5,.5},{1,1}}, {{0,0},{.5,.5},{1,1}}, fit, rms, maximum, error), "共线样本拒绝");
        std::cout << "Pointer core checks passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
