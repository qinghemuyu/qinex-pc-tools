#pragma once
#include "android_tools_service.h"

namespace qpointer::android {
struct CursorProfile {
    const char* name;
    std::vector<std::string> icons;
};
CursorProfile SelectProfile(int index, const std::string& resources);
struct OverlayPolicy { std::string targetName, partition = "product"; bool publicPolicy = false; };
OverlayPolicy ResolvePolicy(const CursorProfile& profile, const std::string& overlayables);
std::filesystem::path BuildCursorPackage(const Phone& phone, const Request& request,
    const std::atomic<bool>& cancel, const std::function<void(const std::string&)>& progress);
// 只从预乘 BGRA 输出独立手机 PNG，不修改 PC 的图片或热点文件。
void WriteCursorPng(const Request& request, const std::filesystem::path& output, double& hotspotX, double& hotspotY);
void WriteStoredZip(const std::filesystem::path& path, const std::vector<std::pair<std::string, std::string>>& entries);
}
