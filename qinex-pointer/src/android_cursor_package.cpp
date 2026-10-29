#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <bcrypt.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include "android_cursor_package.h"
#include "custom_cursor.h"
#include "embedded_runtime.h"

namespace qpointer::android {
namespace {
std::string ReadBytes(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("无法读取构建文件");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::string Number(double value) { std::ostringstream s; s.imbue(std::locale::classic()); s << std::setprecision(9) << value; return s.str(); }
std::string XmlEscape(const std::string& value) {
    std::string r; for (char c : value) {
        switch (c) { case '&': r += "&amp;"; break; case '<': r += "&lt;"; break; case '>': r += "&gt;"; break;
        case '"': r += "&quot;"; break; case '\'': r += "&apos;"; break; default: r += c; }
    } return r;
}
bool HasResource(const std::string& list, const std::string& name) {
    std::istringstream lines(list); std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line); std::string tag, id, resource; fields >> tag >> id >> resource;
        if (tag == "resource" && resource == "drawable/" + name) return true;
    } return false;
}
uint32_t Crc32(const std::string& s) {
    uint32_t crc = 0xffffffff;
    for (unsigned char c : s) { crc ^= c; for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0); }
    return ~crc;
}
void U16(std::string& s, uint16_t n) { s += static_cast<char>(n); s += static_cast<char>(n >> 8); }
void U32(std::string& s, uint32_t n) { U16(s, static_cast<uint16_t>(n)); U16(s, static_cast<uint16_t>(n >> 16)); }
}
CursorProfile SelectProfile(int index, const std::string& resources) {
    CursorProfile p;
    if (index == 1) p = {"crosshair", {"pointer_crosshair_icon", "pointer_crosshair_large_icon", "pointer_crosshair_vector_icon"}};
    else if (index == 2) p = {"spot-hover", {"pointer_spot_hover_icon", "pointer_spot_hover_large_icon", "pointer_spot_hover_vector_icon"}};
    else throw std::runtime_error("请选择手机实际使用的十字或悬停圆点资源路径");
    if (!HasResource(resources, p.icons[0])) throw std::runtime_error("手机没有该路径的基础图标资源，不能构建覆盖包");
    p.icons.erase(std::remove_if(p.icons.begin(), p.icons.end(), [&](const std::string& icon) { return !HasResource(resources, icon); }), p.icons.end()); return p;
}
OverlayPolicy ResolvePolicy(const CursorProfile& profile, const std::string& overlayables) {
    OverlayPolicy result;
    if (Trim(overlayables).empty()) return result;
    struct Item { std::string name, policies; };
    std::map<std::string, Item> items; std::string name, policies, line; std::istringstream lines(overlayables);
    const auto attr = [](const std::string& s, const std::string& key) {
        const auto start = s.find(key + "=\""); if (start == std::string::npos) return std::string();
        const auto first = start + key.size() + 2, end = s.find('"', first); return s.substr(first, end - first);
    };
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (line.rfind("name=", 0) == 0) name = attr(line, "name");
        else if (line.rfind("policies=", 0) == 0) policies = attr(line, "policies");
        else if (line.rfind("drawable/", 0) == 0) items[line.substr(9)] = {name, policies};
    }
    bool product = true, system = true, publicPolicy = true;
    for (const auto& icon : profile.icons) {
        const auto found = items.find(icon);
        if (found == items.end() || found->second.name.empty()) throw std::runtime_error("目标声明了可覆盖集合，但选定光标不在允许集合中");
        if (!result.targetName.empty() && result.targetName != found->second.name) throw std::runtime_error("光标分支属于不同覆盖集合，需要独立 ROM 适配");
        result.targetName = found->second.name;
        const auto& flags = found->second.policies;
        const bool open = flags.find("public") != std::string::npos;
        publicPolicy &= open; product &= open || flags.find("product") != std::string::npos;
        system &= open || flags.find("system") != std::string::npos;
    }
    if (!product && !system) throw std::runtime_error("目标光标要求签名或其它受信策略，当前模块不能满足");
    result.partition = product ? "product" : "system"; result.publicPolicy = publicPolicy; return result;
}
void WriteStoredZip(const std::filesystem::path& path, const std::vector<std::pair<std::string, std::string>>& entries) {
    if (entries.empty() || entries.size() > 65535) throw std::runtime_error("ZIP 条目数无效");
    std::string output, directory;
    for (const auto& [name, data] : entries) {
        if (name.empty() || name.size() > 65535 || name[0] == '/' || name.find("..") != std::string::npos || name.find('\\') != std::string::npos || data.size() > 0xffffffffu)
            throw std::runtime_error("ZIP 路径或大小无效");
        const auto offset = static_cast<uint32_t>(output.size()), size = static_cast<uint32_t>(data.size()), crc = Crc32(data);
        U32(output, 0x04034b50); U16(output, 20); U16(output, 0x0800); U16(output, 0);
        U16(output, 0); U16(output, 0x21); U32(output, crc); U32(output, size); U32(output, size); U16(output, static_cast<uint16_t>(name.size())); U16(output, 0); output += name; output += data;
        U32(directory, 0x02014b50); U16(directory, 20); U16(directory, 20); U16(directory, 0x0800); U16(directory, 0);
        U16(directory, 0); U16(directory, 0x21); U32(directory, crc); U32(directory, size); U32(directory, size); U16(directory, static_cast<uint16_t>(name.size()));
        U16(directory, 0); U16(directory, 0); U16(directory, 0); U16(directory, 0); U32(directory, 0); U32(directory, offset); directory += name;
    }
    const auto offset = static_cast<uint32_t>(output.size()); output += directory;
    U32(output, 0x06054b50); U16(output, 0); U16(output, 0); U16(output, static_cast<uint16_t>(entries.size())); U16(output, static_cast<uint16_t>(entries.size()));
    U32(output, static_cast<uint32_t>(directory.size())); U32(output, offset); U16(output, 0); AtomicWrite(path, output);
}
void WriteCursorPng(const Request& request, const std::filesystem::path& output, double& hx, double& hy) {
    if (request.sizeDp < 16 || request.sizeDp > 96 || !std::isfinite(request.hotspotX) || !std::isfinite(request.hotspotY) || request.hotspotX < 0 || request.hotspotX > 1 || request.hotspotY < 0 || request.hotspotY > 1) throw std::runtime_error("图片大小或热点无效");
    std::wstring error; auto image = LoadCustomCursorImage(request.image.wstring(), error);
    if (!image) throw std::runtime_error(ToUtf8(error));
    const double factor = double(request.sizeDp) / std::max(image->width, image->height);
    const int width = std::max(1, static_cast<int>(std::lround(image->width * factor))), height = std::max(1, static_cast<int>(std::lround(image->height * factor)));
    Gdiplus::Bitmap source(image->width, image->height, image->stride, PixelFormat32bppPARGB, const_cast<BYTE*>(image->premultipliedBgra.data()));
    Gdiplus::Bitmap target(width, height, PixelFormat32bppARGB); Gdiplus::Graphics graphics(&target);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy); graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    if (graphics.DrawImage(&source, 0, 0, width, height) != Gdiplus::Ok) throw std::runtime_error("图片缩放失败");
    UINT count = 0, bytes = 0; Gdiplus::GetImageEncodersSize(&count, &bytes); std::vector<unsigned char> storage(bytes);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (!bytes || Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) throw std::runtime_error("PNG 编码器不可用");
    bool saved = false;
    for (UINT i = 0; i < count; ++i) if (wcscmp(codecs[i].MimeType, L"image/png") == 0) saved = target.Save(output.c_str(), &codecs[i].Clsid, nullptr) == Gdiplus::Ok;
    if (!saved) throw std::runtime_error("手机光标 PNG 保存失败");
    hx = request.hotspotX * (width - 1); hy = request.hotspotY * (height - 1);
}
std::filesystem::path BuildCursorPackage(const Phone& phone, const Request& request, const std::atomic<bool>& cancel,
    const std::function<void(const std::string&)>& progress) {
    const auto profile = SelectProfile(request.profile, phone.resources);
    const auto policy = ResolvePolicy(profile, phone.overlayables);
    progress("释放内嵌资源编译与签名工具…");
    const auto runtime = EnsureRuntime("tools", cancel); EnsureRuntime("java", cancel);
    const auto root = PhoneDirectory(phone), directory = root / (L"candidate-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(directory / L"res/drawable-mdpi");
    std::filesystem::create_directories(directory / L"res/drawable"); std::filesystem::create_directories(directory / L"res/xml");
    // SDK 原生工具对非 ASCII argv 路径的处理存在版本差异，使用宽字符工作目录
    // 和固定 ASCII 相对路径；原始手机资源仍通过宽字符 IO 复制并校验。
    std::filesystem::copy_file(phone.framework, directory / L"framework.apk");
    if (FileHash(directory / L"framework.apk") != phone.frameworkHash) throw std::runtime_error("手机资源副本校验失败");
    const auto run = [&](const std::filesystem::path& exe, std::vector<std::wstring> args, unsigned long timeout = 90000) {
        const auto result = RunProcess(exe, args, cancel, timeout, {}, true, directory);
        if (!result.Ok() || result.truncated) throw std::runtime_error("资源构建阶段失败：" + result.output + (result.cancelled ? "\n操作已取消" : result.timedOut ? "\n工具超时" : ""));
        return result.output;
    };
    double hx = 0, hy = 0; WriteCursorPng(request, directory / L"res/drawable-mdpi/qinex_cursor.png", hx, hy);
    const auto icon = "<pointer-icon xmlns:android=\"http://schemas.android.com/apk/res/android\" android:bitmap=\"@drawable/qinex_cursor\" android:hotSpotX=\"" + Number(hx) + "dp\" android:hotSpotY=\"" + Number(hy) + "dp\"/>";
    std::string mapping = "<overlay>";
    for (const auto& name : profile.icons) {
        AtomicWrite(directory / L"res/drawable" / (ToWide(name) + L".xml"), icon);
        mapping += "<item target=\"drawable/" + name + "\" value=\"@drawable/" + name + "\"/>";
    }
    AtomicWrite(directory / L"res/xml/overlays.xml", mapping + "</overlay>");
    const auto targetName = policy.targetName.empty() ? "" : " android:targetName=\"" + XmlEscape(policy.targetName) + "\"";
    AtomicWrite(directory / L"AndroidManifest.xml", "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" package=\"" + std::string(kOverlayPackage) + "\" android:versionCode=\"1\" android:versionName=\"1\"><uses-sdk android:minSdkVersion=\"29\" android:targetSdkVersion=\"34\"/><overlay android:targetPackage=\"android\"" + targetName + " android:resourcesMap=\"@xml/overlays\" android:isStatic=\"false\" android:priority=\"999\"/><application android:hasCode=\"false\" android:label=\"QInEX Stylus Cursor\"/></manifest>");
    progress("编译光标图片、热点和资源映射…");
    const auto aapt = runtime / L"tools/aapt2.exe";
    run(aapt, {L"compile", L"--dir", L"res", L"-o", L"compiled.zip"});
    run(aapt, {L"link", L"-I", L"framework.apk", L"--manifest", L"AndroidManifest.xml", L"--no-resource-deduping", L"--no-resource-removal", L"-o", L"unsigned.apk", L"compiled.zip"});
    run(runtime / L"tools/zipalign.exe", {L"-f", L"4", L"unsigned.apk", L"aligned.apk"});
    const auto signing = DataDirectory() / L"signing"; std::filesystem::create_directories(signing);
    const auto password = signing / L"password.txt", key = signing / L"cursor.p12";
    if (!std::filesystem::exists(password)) {
        if (std::filesystem::exists(key)) throw std::runtime_error("原签名口令缺失，不能替换已有签名身份");
        unsigned char random[32]; if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) throw std::runtime_error("签名口令生成失败");
        std::string text; const char* hex = "0123456789abcdef"; for (auto c : random) { text += hex[c >> 4]; text += hex[c & 15]; }
        AtomicWrite(password, text + '\n');
    }
    progress("生成或复用本机专用签名，并校验 APK…");
    if (!std::filesystem::exists(key)) run(runtime / L"java/bin/keytool.exe", {L"-genkeypair", L"-alias", L"cursor", L"-keystore", key.wstring(), L"-storetype", L"PKCS12", L"-storepass:file", password.wstring(), L"-keypass:file", password.wstring(), L"-keyalg", L"RSA", L"-keysize", L"2048", L"-validity", L"7300", L"-dname", L"CN=QInEX Pointer, O=QInEX", L"-noprompt"});
    const auto apk = directory / L"QInEXStylus.apk";
    run(runtime / L"java/bin/java.exe", {L"-jar", (runtime / L"tools/lib/apksigner.jar").wstring(), L"sign", L"--ks", key.wstring(), L"--ks-key-alias", L"cursor", L"--ks-pass", L"file:" + password.wstring(), L"--out", apk.wstring(), (directory / L"aligned.apk").wstring()});
    run(runtime / L"java/bin/java.exe", {L"-jar", (runtime / L"tools/lib/apksigner.jar").wstring(), L"verify", L"--verbose", apk.wstring()});
    run(runtime / L"tools/zipalign.exe", {L"-c", L"4", L"QInEXStylus.apk"});
    if (cancel.load()) throw std::runtime_error("操作已取消");
    const auto overlayPath = policy.partition == "system" ? "system/overlay/QInEXStylus/QInEXStylus.apk" : "system/product/overlay/QInEXStylus/QInEXStylus.apk";
    const std::string service = "#!/system/bin/sh\nMODDIR=${0%/*}\n[ -f \"$MODDIR/disable\" ] && exit 0\nn=0\nwhile [ \"$(getprop sys.boot_completed)\" != 1 ] && [ $n -lt 120 ]; do sleep 1; n=$((n+1)); done\ncmd overlay enable --user 0 " + std::string(kOverlayPackage) + " > \"$MODDIR/overlay-result.txt\" 2>&1\ncmd overlay dump " + kOverlayPackage + " >> \"$MODDIR/overlay-result.txt\" 2>&1\n";
    const auto zip = directory / L"cursor.zip";
    WriteStoredZip(zip, {{"module.prop", "id=" + std::string(kModuleId) + "\nname=QInEX Stylus Cursor\nversion=1\nversionCode=1\nauthor=QInEX Pointer\ndescription=Android stylus resource overlay\n"},
        {"customize.sh", "#!/system/bin/sh\nset_perm_recursive \"$MODPATH\" 0 0 0755 0644\nset_perm \"$MODPATH/service.sh\" 0 0 0755\n"},
        {"service.sh", service}, {overlayPath, ReadBytes(apk)},
        {"适配说明.txt", "系统：" + phone.fingerprint + "\n资源摘要：" + phone.frameworkHash + "\n目标：" + profile.name + "\n模块安装后重启，再检查覆盖状态与手机实际光标。默认空图标不能由资源覆盖改变。\n"}});
    // 候选全部校验后才提交；保留上一候选便于诊断和恢复。
    if (std::filesystem::exists(root / L"cursor.zip")) AtomicWrite(root / L"previous-cursor.zip", ReadBytes(root / L"cursor.zip"));
    AtomicWrite(root / L"cursor.zip", ReadBytes(zip)); AtomicWrite(root / L"QInEXStylus.apk", ReadBytes(apk));
    AtomicWrite(root / L"package-phone.txt", SerializePhone(phone));
    AtomicWrite(root / L"package-framework.txt", phone.frameworkPath + '\n' + phone.frameworkHash + '\n');
    AtomicWrite(root / L"package-hashes.txt", FileHash(root / L"cursor.zip") + '\n' + FileHash(root / L"QInEXStylus.apk") + '\n');
    AtomicWrite(root / L"package-policy.txt", policy.publicPolicy ? "public" : "system");
    if (!request.exportPath.empty()) AtomicWrite(request.exportPath, ReadBytes(zip));
    return root / L"cursor.zip";
}
}
