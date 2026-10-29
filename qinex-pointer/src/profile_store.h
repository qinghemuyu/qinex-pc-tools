#pragma once
#include <map>
#include <optional>
#include "settings.h"

// 设备档案只绑定明确的 IPv4 来源。自动发现期间的空过滤条件不代表某台设备，
// 因此不能用于查找或保存校准。该类由 UI 线程拥有，不提供并发写入保证。
class ProfileStore {
public:
    bool Remember(const Settings& settings) {
        auto key = SourceKey(settings);
        if (!key) return false;
        profiles[*key] = settings;
        return true;
    }

    std::optional<Settings> Find(const Settings& source) const {
        auto key = SourceKey(source);
        if (!key) return std::nullopt;
        auto found = profiles.find(*key);
        return found == profiles.end() ? std::nullopt : std::optional<Settings>(found->second);
    }

    // directory 为专用档案目录。加载只读取固定命名格式的文件，并再次核对文件
    // 内部的来源身份；损坏或被改名的文件不会把另一台设备的校准带入当前设备。
    void Load(const std::wstring& directory) {
        profiles.clear();
        std::error_code error;
        std::filesystem::directory_iterator entries(std::filesystem::path(directory), error);
        if (error) return;
        const std::filesystem::directory_iterator end;
        while (entries != end) {
            const auto entry = *entries;
            const auto name = entry.path().filename().wstring();
            if (SafeProfileName(name) && entry.is_regular_file(error) && !error) {
                auto loaded = LoadSettings(entry.path().wstring());
                auto key = SourceKey(loaded);
                if (key && *key == name && HasSourceFields(entry.path().wstring())) profiles[*key] = loaded;
            }
            error.clear();
            entries.increment(error);
            if (error) break;
        }
    }

    // 每个设备独立原子替换，其他设备与目录中的其他文件均保留。
    // 返回失败时出错设备的旧文件保持完整；调用方保留当前编辑状态以便重试。
    bool Save(const std::wstring& directory, std::wstring& error) const {
        error.clear();
        std::error_code filesystemError;
        const std::filesystem::path root(directory);
        if (!std::filesystem::create_directories(root, filesystemError) && filesystemError) {
            error = L"无法创建设备配置目录，请检查文件夹写入权限。";
            return false;
        }
        for (const auto& profile : profiles) {
            const auto destination = root / profile.first;
            if (std::filesystem::exists(destination, filesystemError)) {
                const auto existing = SourceKey(LoadSettings(destination.wstring()));
                if (!HasSourceFields(destination.wstring()) || !existing || *existing != profile.first) {
                    error = L"设备配置文件的来源信息不匹配，已保留原文件。";
                    return false;
                }
            } else if (filesystemError) {
                error = L"无法检查已有设备配置，请检查文件夹访问权限。";
                return false;
            }
            if (!SaveSettings(profile.second, destination.wstring())) {
                error = L"设备配置保存失败，原配置已保留，请检查文件夹写入权限。";
                return false;
            }
        }
        return true;
    }

private:
    std::map<std::wstring, Settings> profiles;

    static bool HasSourceFields(const std::wstring& file) {
        // 普通配置加载会回退损坏字段，但设备身份不能用默认值补齐后继续复用校准。
        const double source = ReadNumber(L"source", -1, file);
        const double port = ReadNumber(L"port", -1, file);
        return (source == 0 || source == 1) && port >= 1 && port <= 65535 && port == std::floor(port) &&
            (source != 1 || port == 6533) && !ReadIni(L"sourceIp", L"", file).empty();
    }

    static bool SafeProfileName(const std::wstring& name) {
        // device-0-1985-7f000001.ini，命名不包含用户输入的标题、目录或原始 IP 文本。
        if (name.size() != 26 || name.compare(0, 7, L"device-") != 0 ||
            (name[7] != L'0' && name[7] != L'1') || name[8] != L'-' || name[13] != L'-' ||
            name.compare(22, 4, L".ini") != 0) return false;
        for (size_t index : {size_t(9), size_t(10), size_t(11), size_t(12), size_t(14), size_t(15),
            size_t(16), size_t(17), size_t(18), size_t(19), size_t(20), size_t(21)}) {
            if (!((name[index] >= L'0' && name[index] <= L'9') || (name[index] >= L'a' && name[index] <= L'f'))) return false;
        }
        return true;
    }

    static std::optional<std::wstring> SourceKey(const Settings& source) {
        if ((source.source != 0 && source.source != 1) || source.port < 1 || source.port > 65535 ||
            (source.source == 1 && source.port != 6533)) return std::nullopt;
        const auto& text = source.sourceIp;
        size_t position = 0;
        unsigned long address = 0;
        for (int part = 0; part < 4; ++part) {
            const size_t first = position;
            unsigned int value = 0;
            while (position < text.size() && text[position] >= L'0' && text[position] <= L'9') {
                value = value * 10 + static_cast<unsigned int>(text[position] - L'0');
                if (++position - first > 3 || value > 255) return std::nullopt;
            }
            if (position == first || (position - first > 1 && text[first] == L'0')) return std::nullopt;
            address = (address << 8) | value;
            if (part < 3) {
                if (position == text.size() || text[position++] != L'.') return std::nullopt;
            }
        }
        if (position != text.size() || address == 0 || address == 0xffffffffUL) return std::nullopt;
        std::wostringstream name;
        name.imbue(std::locale::classic());
        name << L"device-" << source.source << L'-' << std::hex << std::setfill(L'0') <<
            std::setw(4) << source.port << L'-' << std::setw(8) << address << L".ini";
        return name.str();
    }
};
