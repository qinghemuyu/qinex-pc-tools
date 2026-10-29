#include "pointer_core.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <utility>

namespace qpointer {

Rect DisplayRect(const Geometry& g) {
    if (g.stretch) return g.target;
    double width = g.frameWidth * g.crop.w, height = g.frameHeight * g.crop.h;
    if (g.rotation == 90 || g.rotation == 270) std::swap(width, height);
    const double scale = std::min(g.target.w / width, g.target.h / height);
    width *= scale;
    height *= scale;
    return {g.target.x + (g.target.w - width) * 0.5,
            g.target.y + (g.target.h - height) * 0.5, width, height};
}

namespace {
constexpr double kEdgeTolerance = 1e-10;

bool Finite(Point p) { return std::isfinite(p.x) && std::isfinite(p.y); }
bool Unit(double value) { return std::isfinite(value) && value >= 0 && value <= 1; }
bool InsideUnit(double value) {
    return std::isfinite(value) && value >= -kEdgeTolerance && value <= 1 + kEdgeTolerance;
}
double ClampUnit(double value) { return std::clamp(value, 0.0, 1.0); }

bool ValidAffine(const Affine& a) {
    if (!std::isfinite(a.a) || !std::isfinite(a.b) || !std::isfinite(a.c) ||
        !std::isfinite(a.d) || !std::isfinite(a.e) || !std::isfinite(a.f)) return false;
    const double scale = std::max({std::abs(a.a), std::abs(a.b), std::abs(a.d), std::abs(a.e)});
    if (scale <= 0) return false;
    const double determinant = (a.a / scale) * (a.e / scale) - (a.b / scale) * (a.d / scale);
    return std::isfinite(determinant) && std::abs(determinant) > 1e-8;
}

bool ValidRoi(const Rect& r) {
    return Unit(r.x) && Unit(r.y) && std::isfinite(r.w) && std::isfinite(r.h) &&
           r.w > 0 && r.h > 0 && r.x + r.w <= 1 + kEdgeTolerance &&
           r.y + r.h <= 1 + kEdgeTolerance;
}

Point Rotate(Point p, int rotation) {
    switch (rotation) {
    case 90: return {1 - p.y, p.x};
    case 180: return {1 - p.x, 1 - p.y};
    case 270: return {p.y, 1 - p.x};
    default: return p;
    }
}

// 报文长度和递归深度有上限；未知字段也完整验证，不能借扩展字段绕过 JSON 语法校验。
class JsonReader {
public:
    explicit JsonReader(const std::string& text) : text_(text) {}

    bool Parse(Packet& packet, std::string& error) {
        if (text_.empty() || text_.size() > 4096) return Fail(error, "JSON 报文长度必须在 1～4096 字节之间");
        if (!ValidUtf8()) return Fail(error, "JSON 报文必须是有效 UTF-8");
        if (!Consume('{')) return Fail(error, "JSON 顶层必须是对象");
        Packet parsed;
        unsigned fields = 0;
        if (Consume('}')) return Fail(error, "缺少 type 字段");
        while (true) {
            std::string key;
            if (!ReadString(key) || !Consume(':')) return Fail(error, "JSON 对象字段格式错误");
            unsigned bit = key == "type" ? 1 : key == "nx" ? 2 : key == "ny" ? 4 : key == "show" ? 8 : key == "down" ? 16 : 0;
            if (bit && (fields & bit)) return Fail(error, "协议字段不能重复");
            fields |= bit;
            if (key == "type") {
                std::string type;
                if (!ReadString(type)) return Fail(error, "type 必须是字符串");
                if (type == "move") parsed.kind = PacketKind::Move;
                else if (type == "show") parsed.kind = PacketKind::Show;
                else if (type == "hide") parsed.kind = PacketKind::Hide;
                else if (type == "heartbeat") parsed.kind = PacketKind::Heartbeat;
                else return Fail(error, "未知的光标消息类型");
            } else if (key == "nx" || key == "ny") {
                double value;
                if (!ReadNumber(value) || !Unit(value)) return Fail(error, "nx/ny 必须是 0～1 的有限数值");
                (key == "nx" ? parsed.nx : parsed.ny) = value;
            } else if (key == "show" || key == "down") {
                bool value;
                if (!ReadBool(value)) return Fail(error, "show/down 必须是布尔值");
                (key == "show" ? parsed.show : parsed.down) = value;
            } else if (!SkipValue(0)) {
                return Fail(error, "扩展字段包含无效 JSON 或嵌套过深");
            }
            if (Consume('}')) break;
            if (!Consume(',')) return Fail(error, "JSON 对象缺少逗号或结束符");
        }
        SkipWhitespace();
        if (position_ != text_.size()) return Fail(error, "JSON 对象后包含多余数据");
        if (!(fields & 1)) return Fail(error, "缺少 type 字段");
        packet = parsed;
        error.clear();
        return true;
    }

private:
    static bool Fail(std::string& error, const char* message) { error = message; return false; }
    void SkipWhitespace() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++position_;
        }
    }
    bool Consume(char expected) {
        SkipWhitespace();
        if (position_ >= text_.size() || text_[position_] != expected) return false;
        ++position_;
        return true;
    }
    bool Literal(const char* word) {
        SkipWhitespace();
        std::size_t length = 0;
        while (word[length]) ++length;
        if (text_.compare(position_, length, word) != 0) return false;
        position_ += length;
        return true;
    }
    bool ReadBool(bool& value) {
        if (Literal("true")) { value = true; return true; }
        if (Literal("false")) { value = false; return true; }
        return false;
    }
    static bool Digit(char c) { return c >= '0' && c <= '9'; }
    bool ReadNumber(double& value) {
        SkipWhitespace();
        const std::size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        if (position_ >= text_.size()) return false;
        if (text_[position_] == '0') ++position_;
        else {
            if (text_[position_] < '1' || text_[position_] > '9') return false;
            while (position_ < text_.size() && Digit(text_[position_])) ++position_;
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            const std::size_t fraction = position_;
            while (position_ < text_.size() && Digit(text_[position_])) ++position_;
            if (fraction == position_) return false;
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+')) ++position_;
            const std::size_t exponent = position_;
            while (position_ < text_.size() && Digit(text_[position_])) ++position_;
            if (exponent == position_) return false;
        }
        const auto result = std::from_chars(text_.data() + start, text_.data() + position_, value);
        return result.ec == std::errc{} && result.ptr == text_.data() + position_ && std::isfinite(value);
    }
    bool ReadHex(unsigned& value) {
        value = 0;
        for (int i = 0; i < 4; ++i) {
            if (position_ >= text_.size()) return false;
            const char c = text_[position_++];
            const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0) return false;
            value = value * 16 + static_cast<unsigned>(digit);
        }
        return true;
    }
    static void AppendUtf8(std::string& output, unsigned code) {
        if (code < 0x80) output.push_back(static_cast<char>(code));
        else if (code < 0x800) {
            output.push_back(static_cast<char>(0xc0 | (code >> 6)));
            output.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code < 0x10000) {
            output.push_back(static_cast<char>(0xe0 | (code >> 12)));
            output.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (code >> 18)));
            output.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }
    bool ReadString(std::string& output) {
        if (!Consume('"')) return false;
        output.clear();
        while (position_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[position_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') { output.push_back(static_cast<char>(c)); continue; }
            if (position_ >= text_.size()) return false;
            switch (text_[position_++]) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                unsigned code;
                if (!ReadHex(code)) return false;
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (position_ + 2 > text_.size() || text_[position_] != '\\' || text_[position_ + 1] != 'u') return false;
                    position_ += 2;
                    unsigned low;
                    if (!ReadHex(low) || low < 0xdc00 || low > 0xdfff) return false;
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff) return false;
                AppendUtf8(output, code);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool SkipValue(unsigned depth) {
        if (depth > 16) return false;
        SkipWhitespace();
        if (position_ >= text_.size()) return false;
        if (text_[position_] == '"') { std::string value; return ReadString(value); }
        if (Consume('{')) {
            if (Consume('}')) return true;
            while (true) {
                std::string key;
                if (!ReadString(key) || !Consume(':') || !SkipValue(depth + 1)) return false;
                if (Consume('}')) return true;
                if (!Consume(',')) return false;
            }
        }
        if (Consume('[')) {
            if (Consume(']')) return true;
            while (true) {
                if (!SkipValue(depth + 1)) return false;
                if (Consume(']')) return true;
                if (!Consume(',')) return false;
            }
        }
        if (Literal("true") || Literal("false") || Literal("null")) return true;
        double value;
        return ReadNumber(value);
    }
    bool ValidUtf8() const {
        for (std::size_t i = 0; i < text_.size();) {
            const unsigned char first = static_cast<unsigned char>(text_[i++]);
            if (first < 0x80) continue;
            unsigned code, continuation, minimum;
            if (first >= 0xc2 && first <= 0xdf) { code = first & 0x1f; continuation = 1; minimum = 0x80; }
            else if (first >= 0xe0 && first <= 0xef) { code = first & 0x0f; continuation = 2; minimum = 0x800; }
            else if (first >= 0xf0 && first <= 0xf4) { code = first & 7; continuation = 3; minimum = 0x10000; }
            else return false;
            if (i + continuation > text_.size()) return false;
            while (continuation--) {
                const unsigned char next = static_cast<unsigned char>(text_[i++]);
                if ((next & 0xc0) != 0x80) return false;
                code = (code << 6) | (next & 0x3f);
            }
            if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
        }
        return true;
    }

    const std::string& text_;
    std::size_t position_ = 0;
};
}  // namespace

bool ValidateGeometry(const Geometry& g, std::string& error) {
    if (!std::isfinite(g.target.x) || !std::isfinite(g.target.y) ||
        !std::isfinite(g.target.w) || !std::isfinite(g.target.h) ||
        !std::isfinite(g.target.x + g.target.w) || !std::isfinite(g.target.y + g.target.h) ||
        g.target.w < 2 || g.target.h < 2) {
        error = "目标区域宽高必须至少为 2 个有限物理像素";
        return false;
    }
    if (!std::isfinite(g.frameWidth) || !std::isfinite(g.frameHeight) || g.frameWidth <= 0 || g.frameHeight <= 0) {
        error = "完整采集帧宽高必须为有限正数";
        return false;
    }
    if (!ValidRoi(g.content) || !ValidRoi(g.crop)) {
        error = "内容和裁剪矩形必须位于完整采集帧内，宽高必须大于 0";
        return false;
    }
    if (std::min(g.content.x + g.content.w, g.crop.x + g.crop.w) <= std::max(g.content.x, g.crop.x) ||
        std::min(g.content.y + g.content.h, g.crop.y + g.crop.h) <= std::max(g.content.y, g.crop.y)) {
        error = "裁剪区域必须包含部分手机内容";
        return false;
    }
    if (g.rotation != 0 && g.rotation != 90 && g.rotation != 180 && g.rotation != 270) {
        error = "预览旋转角度只能为 0、90、180 或 270";
        return false;
    }
    if (!ValidAffine(g.calibration)) {
        error = "校准矩阵包含无效数值或接近退化";
        return false;
    }
    const Rect display = DisplayRect(g);
    if (!std::isfinite(display.x) || !std::isfinite(display.y) || !std::isfinite(display.w) ||
        !std::isfinite(display.h) || display.w < 2 || display.h < 2) {
        error = "等比适配后的采集画面宽高必须至少为 2 个物理像素";
        return false;
    }
    error.clear();
    return true;
}

std::optional<Point> MapPoint(const Geometry& g, Point input) {
    std::string error;
    if (!ValidateGeometry(g, error) || !Unit(input.x) || !Unit(input.y)) return std::nullopt;
    Point p{g.calibration.a * input.x + g.calibration.b * input.y + g.calibration.c,
            g.calibration.d * input.x + g.calibration.e * input.y + g.calibration.f};
    if (!InsideUnit(p.x) || !InsideUnit(p.y)) return std::nullopt;
    p = {g.content.x + ClampUnit(p.x) * g.content.w, g.content.y + ClampUnit(p.y) * g.content.h};
    p = {(p.x - g.crop.x) / g.crop.w, (p.y - g.crop.y) / g.crop.h};
    if (!InsideUnit(p.x) || !InsideUnit(p.y)) return std::nullopt;
    p = Rotate({ClampUnit(p.x), ClampUnit(p.y)}, g.rotation);
    if (g.mirrorX) p.x = 1 - p.x;
    if (g.mirrorY) p.y = 1 - p.y;
    const Rect d = DisplayRect(g);
    p = {d.x + p.x * (d.w - 1), d.y + p.y * (d.h - 1)};
    return Finite(p) ? std::optional<Point>{p} : std::nullopt;
}

std::optional<Point> UnmapPoint(const Geometry& g, Point screen) {
    std::string error;
    if (!ValidateGeometry(g, error) || !Finite(screen)) return std::nullopt;
    const Rect d = DisplayRect(g);
    Point p{(screen.x - d.x) / (d.w - 1), (screen.y - d.y) / (d.h - 1)};
    if (!InsideUnit(p.x) || !InsideUnit(p.y)) return std::nullopt;
    p = {ClampUnit(p.x), ClampUnit(p.y)};
    if (g.mirrorX) p.x = 1 - p.x;
    if (g.mirrorY) p.y = 1 - p.y;
    p = Rotate(p, (360 - g.rotation) % 360);
    p = {g.crop.x + p.x * g.crop.w, g.crop.y + p.y * g.crop.h};
    p = {(p.x - g.content.x) / g.content.w, (p.y - g.content.y) / g.content.h};
    if (!InsideUnit(p.x) || !InsideUnit(p.y)) return std::nullopt;
    return Point{ClampUnit(p.x), ClampUnit(p.y)};
}

bool FitRangeCalibration(const Geometry& geometry, Rect screenRange,
                         Affine& result, std::string& error) {
    Geometry base = geometry;
    base.calibration = {};
    if (!ValidateGeometry(base, error)) return false;
    const Rect display = DisplayRect(base);
    constexpr double tolerance = 0.75;
    if (!std::isfinite(screenRange.x) || !std::isfinite(screenRange.y) ||
        !std::isfinite(screenRange.w) || !std::isfinite(screenRange.h) ||
        screenRange.w < 2 || screenRange.h < 2 ||
        screenRange.x < display.x - tolerance || screenRange.y < display.y - tolerance ||
        screenRange.x + screenRange.w > display.x + display.w + tolerance ||
        screenRange.y + screenRange.h > display.y + display.h + tolerance) {
        error = "快捷校准范围必须完整位于实际视频画面内";
        return false;
    }

    const std::vector<Point> input{{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    std::vector<Point> observed;
    observed.reserve(input.size());
    for (const Point corner : input) {
        const auto original = MapPoint(base, corner);
        if (!original) {
            error = "无法计算当前画面的边角位置";
            return false;
        }
        const double x = (original->x - display.x) / (display.w - 1);
        const double y = (original->y - display.y) / (display.h - 1);
        const Point desired{screenRange.x + x * (screenRange.w - 1),
                            screenRange.y + y * (screenRange.h - 1)};
        const auto normalized = UnmapPoint(base, desired);
        if (!normalized) {
            error = "快捷校准范围超出了实际视频画面";
            return false;
        }
        observed.push_back(*normalized);
    }

    double rms = 0, maxError = 0;
    return FitAffine(input, observed, result, rms, maxError, error);
}

bool FitAffine(const std::vector<Point>& input, const std::vector<Point>& observed,
               Affine& result, double& rms, double& maxError, std::string& error) {
    if (input.size() != observed.size() || input.size() < 3 || input.size() > 65536) {
        error = "校准需要 3～65536 组完整点对";
        return false;
    }
    const std::size_t count = input.size();
    Point center{}, outputCenter{};
    for (std::size_t i = 0; i < count; ++i) {
        if (!Finite(input[i]) || !Finite(observed[i])) { error = "校准点必须为有限数值"; return false; }
        const double weight = 1.0 / static_cast<double>(i + 1);
        center.x += (input[i].x - center.x) * weight;
        center.y += (input[i].y - center.y) * weight;
        outputCenter.x += (observed[i].x - outputCenter.x) * weight;
        outputCenter.y += (observed[i].y - outputCenter.y) * weight;
    }
    double squaredRadius = 0;
    for (Point p : input) squaredRadius += (p.x - center.x) * (p.x - center.x) + (p.y - center.y) * (p.y - center.y);
    const double scale = std::sqrt(squaredRadius / static_cast<double>(count));
    if (!std::isfinite(scale) || scale < 1e-9) { error = "校准采样点过于集中，无法稳定拟合"; return false; }

    // 先中心化并统一缩放，再用带列主元的 Householder QR 解最小二乘，避免正规方程放大病态性。
    std::vector<std::array<double, 3>> matrix(count);
    std::vector<Point> rhs(count);
    for (std::size_t i = 0; i < count; ++i) {
        matrix[i] = {(input[i].x - center.x) / scale, (input[i].y - center.y) / scale, 1};
        rhs[i] = {observed[i].x - outputCenter.x, observed[i].y - outputCenter.y};
    }
    std::array<int, 3> permutation{0, 1, 2};
    for (int column = 0; column < 3; ++column) {
        int pivot = column;
        double best = -1;
        for (int candidate = column; candidate < 3; ++candidate) {
            double norm = 0;
            for (std::size_t row = column; row < count; ++row) norm += matrix[row][candidate] * matrix[row][candidate];
            if (norm > best) { best = norm; pivot = candidate; }
        }
        const double norm = std::sqrt(best);
        if (!std::isfinite(norm) || norm < 1e-6 * std::sqrt(static_cast<double>(count))) {
            error = "校准采样点共线或接近共线，请重新分散采样";
            return false;
        }
        if (pivot != column) {
            for (auto& row : matrix) std::swap(row[pivot], row[column]);
            std::swap(permutation[pivot], permutation[column]);
        }
        std::vector<double> reflector(count - column);
        for (std::size_t row = column; row < count; ++row) reflector[row - column] = matrix[row][column];
        const double diagonal = matrix[column][column] >= 0 ? -norm : norm;
        reflector[0] -= diagonal;
        double divisor = 0;
        for (double v : reflector) divisor += v * v;
        const double beta = 2 / divisor;
        for (int targetColumn = column; targetColumn < 3; ++targetColumn) {
            double dot = 0;
            for (std::size_t row = column; row < count; ++row) dot += reflector[row - column] * matrix[row][targetColumn];
            for (std::size_t row = column; row < count; ++row) matrix[row][targetColumn] -= beta * dot * reflector[row - column];
        }
        Point dot{};
        for (std::size_t row = column; row < count; ++row) {
            dot.x += reflector[row - column] * rhs[row].x;
            dot.y += reflector[row - column] * rhs[row].y;
        }
        for (std::size_t row = column; row < count; ++row) {
            rhs[row].x -= beta * dot.x * reflector[row - column];
            rhs[row].y -= beta * dot.y * reflector[row - column];
        }
        matrix[column][column] = diagonal;
        for (std::size_t row = column + 1; row < count; ++row) matrix[row][column] = 0;
    }
    std::array<Point, 3> solved{}, coefficient{};
    for (int row = 2; row >= 0; --row) {
        Point value = rhs[row];
        for (int column = row + 1; column < 3; ++column) {
            value.x -= matrix[row][column] * solved[column].x;
            value.y -= matrix[row][column] * solved[column].y;
        }
        solved[row] = {value.x / matrix[row][row], value.y / matrix[row][row]};
    }
    for (int i = 0; i < 3; ++i) coefficient[permutation[i]] = solved[i];
    Affine fitted;
    fitted.a = coefficient[0].x / scale;
    fitted.b = coefficient[1].x / scale;
    fitted.c = outputCenter.x + coefficient[2].x - fitted.a * center.x - fitted.b * center.y;
    fitted.d = coefficient[0].y / scale;
    fitted.e = coefficient[1].y / scale;
    fitted.f = outputCenter.y + coefficient[2].y - fitted.d * center.x - fitted.e * center.y;
    if (!ValidAffine(fitted)) { error = "拟合结果退化，无法可靠映射二维触点"; return false; }
    double squaredError = 0, largestError = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const double dx = fitted.a * input[i].x + fitted.b * input[i].y + fitted.c - observed[i].x;
        const double dy = fitted.d * input[i].x + fitted.e * input[i].y + fitted.f - observed[i].y;
        const double distance = std::hypot(dx, dy);
        squaredError += distance * distance;
        largestError = std::max(largestError, distance);
    }
    if (!std::isfinite(squaredError)) { error = "校准残差超出有限数值范围"; return false; }
    result = fitted;
    rms = std::sqrt(squaredError / static_cast<double>(count));
    maxError = largestError;
    error.clear();
    return true;
}

bool ParsePacket(const std::string& json, Packet& packet, std::string& error) {
    return JsonReader(json).Parse(packet, error);
}

bool ApplyPacket(PointerState& state, const Packet& packet, std::uint64_t now) {
    if ((packet.nx && !Unit(*packet.nx)) || (packet.ny && !Unit(*packet.ny))) return false;
    if (packet.kind != PacketKind::Move && packet.kind != PacketKind::Show &&
        packet.kind != PacketKind::Hide && packet.kind != PacketKind::Heartbeat) return false;
    state.lastPacketMs = now;
    if (packet.kind == PacketKind::Heartbeat) return true;
    if (packet.kind == PacketKind::Hide) { state.show = false; state.down = false; return true; }
    if (packet.kind == PacketKind::Show) { state.show = state.hasPosition; return true; }

    if (state.hasPosition || (packet.nx && packet.ny)) {
        if (packet.nx) state.position.x = *packet.nx;
        if (packet.ny) state.position.y = *packet.ny;
        state.hasPosition = true;
    }
    state.show = state.hasPosition && packet.show.value_or(true);
    if (packet.down) state.down = *packet.down;
    if (!state.show) state.down = false;
    return true;
}

}  // namespace qpointer
