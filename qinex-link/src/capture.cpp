#include "capture.h"
#include "app.h"
#include "hid_usage.h"
#include "performance.h"
#include <algorithm>
#include <climits>
#include <thread>
#include <deque>
#include <vector>

namespace {

std::thread s_thread;
DWORD  s_tid = 0;
HHOOK  s_kbHook = nullptr, s_msHook = nullptr;
HWND   s_captureWnd = nullptr;
std::atomic<bool> s_hotkeyDown{false};
std::atomic<ULONGLONG> s_hotkeyLastEventMs{0};
constexpr uint8_t POINTER_KEY_HOOK = 1u << 0;
constexpr uint8_t POINTER_KEY_RAW = 1u << 1;
constexpr uint8_t POINTER_KEY_SWALLOW = 1u << 2;
constexpr UINT POINTER_MODIFIER_MASK = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
constexpr size_t POINTER_HOTKEY_COUNT = 5;
constexpr ULONGLONG POINTER_FLOW_STALE_MS = 2000;
constexpr uint32_t POINTER_FLOW_BITS_MASK = 0xffu;
constexpr unsigned POINTER_FLOW_KEY_SHIFT = 8;

struct PointerHotkeySpec {
    const wchar_t* property;
    UINT fallbackVk;
    UINT fallbackModifiers;
    WPARAM messageId;
    bool needsOutput;
};

struct PointerHotkeyBinding {
    UINT virtualKey = 0;
    UINT modifiers = 0;
};

// 退出优先于其它动作。正常设置会拒绝冲突，这个顺序仍能让损坏配置保留逃生能力。
constexpr PointerHotkeySpec POINTER_HOTKEYS[POINTER_HOTKEY_COUNT] = {
    {L"QInEX.Pointer.Hotkey.Exit.v1",  VK_F11, 0,                     3, true},
    {nullptr,                            VK_F12, MOD_CONTROL | MOD_ALT, 2, true},
    {L"QInEX.Pointer.Hotkey.Range.v1", VK_F8,  0,                     4, true},
    {L"QInEX.Pointer.Hotkey.Hud.v1",   VK_F7,  0,                     5, true},
    {L"QInEX.Pointer.Hotkey.Pause.v1", VK_F10, MOD_CONTROL | MOD_ALT, 1, false},
};

// virtual-key 与来源位必须原子提交。主窗口线程会在换绑/重新激活时清锁存，若拆成
// 两个原子，捕获线程可能夹在两次写入之间留下“有 flow、无 key”的永久状态。
std::atomic<uint32_t> s_pointerFlows[POINTER_HOTKEY_COUNT]{};
std::atomic<ULONGLONG> s_pointerFlowLastEventMs[POINTER_HOTKEY_COUNT]{};

// Hook 与 Raw Input 都在捕获消息线程执行。短缓存避免每个键盘报告都跨进程查窗口属性，
// 同时让 Pointer 在设置页应用新绑定后很快生效。
struct PointerHotkeyCache {
    HWND main = nullptr;
    HWND output = nullptr;
    ULONGLONG refreshAt = 0;
    PointerHotkeyBinding bindings[POINTER_HOTKEY_COUNT]{};
} s_pointerHotkeys;

std::atomic<uint64_t> s_lastRawMouseTimelineUs{0};
uint8_t s_hookMods = 0;   // 仅捕获线程的 LL Hook 回调访问,用于 Ctrl+Alt+Q 应急解锁

constexpr UINT MSG_CAPTURE_HOOKKEY = WM_APP + 1;

UINT pointerModifiers(uint8_t hidModifiers) {
    UINT modifiers = 0;
    if (hidModifiers & 0x11) modifiers |= MOD_CONTROL;
    if (hidModifiers & 0x22) modifiers |= MOD_SHIFT;
    if (hidModifiers & 0x44) modifiers |= MOD_ALT;
    if (hidModifiers & 0x88) modifiers |= MOD_WIN;
    return modifiers;
}

PointerHotkeyBinding readPointerHotkey(HWND pointerMain, const PointerHotkeySpec& spec) {
    if (!spec.property) return {spec.fallbackVk, spec.fallbackModifiers};
    const ULONG_PTR published = reinterpret_cast<ULONG_PTR>(GetPropW(pointerMain, spec.property));
    if (!published) return {spec.fallbackVk, spec.fallbackModifiers};
    // Pointer 用 packed+1 发布，1 明确表示用户关闭；0 留给没有该属性的旧版本。
    const ULONG_PTR packed = published - 1;
    return {
        static_cast<UINT>(packed & 0xffu),
        static_cast<UINT>((packed >> 8) & POINTER_MODIFIER_MASK),
    };
}

void refreshPointerHotkeys() {
    const ULONGLONG now = GetTickCount64();
    if (now < s_pointerHotkeys.refreshAt &&
            (!s_pointerHotkeys.main || IsWindow(s_pointerHotkeys.main))) return;
    s_pointerHotkeys.refreshAt = now + 100;
    s_pointerHotkeys.main = FindWindowW(L"QPointerMain", nullptr);
    s_pointerHotkeys.output = FindWindowW(L"QPointerCaptureOutput", nullptr);
    if (!s_pointerHotkeys.main) {
        for (auto& binding : s_pointerHotkeys.bindings) binding = {};
        s_pointerHotkeys.output = nullptr;
        return;
    }
    for (size_t index = 0; index < POINTER_HOTKEY_COUNT; ++index)
        s_pointerHotkeys.bindings[index] = readPointerHotkey(
            s_pointerHotkeys.main, POINTER_HOTKEYS[index]);
}

uint32_t makePointerFlow(UINT virtualKey, uint8_t bits) {
    return (static_cast<uint32_t>(virtualKey & 0xffu) << POINTER_FLOW_KEY_SHIFT) |
        static_cast<uint32_t>(bits);
}

uint8_t pointerFlowBits(uint32_t state) {
    return static_cast<uint8_t>(state & POINTER_FLOW_BITS_MASK);
}

UINT pointerFlowKey(uint32_t state) {
    return static_cast<UINT>((state >> POINTER_FLOW_KEY_SHIFT) & 0xffu);
}

void clearPointerFlow(size_t index) {
    s_pointerFlows[index].store(0, std::memory_order_release);
    s_pointerFlowLastEventMs[index].store(0, std::memory_order_relaxed);
}

int latchedPointerAction(UINT virtualKey) {
    const ULONGLONG now = GetTickCount64();
    for (size_t index = 0; index < POINTER_HOTKEY_COUNT; ++index) {
        for (;;) {
            uint32_t state = s_pointerFlows[index].load(std::memory_order_acquire);
            if (!pointerFlowBits(state)) break;
            const ULONGLONG last =
                s_pointerFlowLastEventMs[index].load(std::memory_order_relaxed);
            // 时间戳为 0 只可能来自跨线程 reset 的竞争窗口，也按陈旧会话处理。
            if (!last || now - last > POINTER_FLOW_STALE_MS) {
                if (s_pointerFlows[index].compare_exchange_strong(
                        state, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    s_pointerFlowLastEventMs[index].store(0, std::memory_order_relaxed);
                    break;
                }
                continue;
            }
            if (pointerFlowKey(state) == virtualKey)
                return static_cast<int>(index);
            break;
        }
    }
    return -1;
}

// Hook/Raw 都只由捕获消息线程写，另一个线程只会把整份状态清零。CAS 让 reset 与
// 当前物理沿有明确先后关系；返回值是本次更新前的完整状态。
uint32_t addPointerFlow(size_t index, UINT virtualKey, uint8_t bits) {
    auto& flow = s_pointerFlows[index];
    uint32_t previous = flow.load(std::memory_order_acquire);
    for (;;) {
        const uint8_t oldBits = pointerFlowBits(previous);
        if (oldBits && pointerFlowKey(previous) != virtualKey) return previous;
        const uint32_t next = makePointerFlow(virtualKey,
            static_cast<uint8_t>(oldBits | bits));
        if (flow.compare_exchange_weak(previous, next,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            s_pointerFlowLastEventMs[index].store(GetTickCount64(), std::memory_order_relaxed);
            return previous;
        }
    }
}

uint32_t removePointerFlow(size_t index, UINT virtualKey, uint8_t clearBits,
        bool clearSwallowWithoutHook = false) {
    auto& flow = s_pointerFlows[index];
    uint32_t previous = flow.load(std::memory_order_acquire);
    for (;;) {
        const uint8_t oldBits = pointerFlowBits(previous);
        if (!oldBits || pointerFlowKey(previous) != virtualKey) return previous;
        uint8_t bitsToClear = clearBits;
        if (clearSwallowWithoutHook && !(oldBits & POINTER_KEY_HOOK))
            bitsToClear = static_cast<uint8_t>(bitsToClear | POINTER_KEY_SWALLOW);
        const uint8_t remaining = static_cast<uint8_t>(oldBits & ~bitsToClear);
        const uint32_t next = remaining ? makePointerFlow(virtualKey, remaining) : 0;
        if (flow.compare_exchange_weak(previous, next,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            s_pointerFlowLastEventMs[index].store(
                remaining ? GetTickCount64() : 0, std::memory_order_relaxed);
            return previous;
        }
    }
}

int matchingPointerAction(UINT virtualKey, UINT modifiers) {
    refreshPointerHotkeys();
    if (!s_pointerHotkeys.main) return -1;
    for (size_t index = 0; index < POINTER_HOTKEY_COUNT; ++index) {
        const auto& spec = POINTER_HOTKEYS[index];
        const auto& binding = s_pointerHotkeys.bindings[index];
        if (spec.needsOutput && !s_pointerHotkeys.output) continue;
        if (binding.virtualKey && binding.virtualKey == virtualKey &&
                binding.modifiers == (modifiers & POINTER_MODIFIER_MASK))
            return static_cast<int>(index);
    }
    return -1;
}

UINT pointerModifierPrefixMask() {
    refreshPointerHotkeys();
    if (!s_pointerHotkeys.main) return 0;
    UINT mask = 0;
    for (size_t index = 0; index < POINTER_HOTKEY_COUNT; ++index) {
        const auto& spec = POINTER_HOTKEYS[index];
        const auto& binding = s_pointerHotkeys.bindings[index];
        if (!binding.virtualKey || (spec.needsOutput && !s_pointerHotkeys.output)) continue;
        mask |= binding.modifiers;
    }
    return mask & POINTER_MODIFIER_MASK;
}

void consumePointerModifiers(uint8_t hidModifiers);

// 返回 true 表示此沿属于 Pointer，调用方必须结束常规键盘处理；swallow 指示 Hook
// 是否阻止本机继续收到该沿。锁存虚拟键保证动作触发后窗口/独占状态立刻变化时，抬起沿
// 仍按同一次会话处理，不会漏发到 P4。
bool handlePointerHook(UINT virtualKey, bool up, uint8_t hidModifiers, bool& swallow) {
    const bool grabbed = g.grabbed.load(std::memory_order_relaxed);
    int action = latchedPointerAction(virtualKey);
    if (action < 0 && !up && grabbed)
        action = matchingPointerAction(virtualKey, pointerModifiers(hidModifiers));
    if (action < 0) return false;

    uint32_t state = s_pointerFlows[action].load(std::memory_order_acquire);
    uint8_t flow = pointerFlowBits(state);
    // Raw 在共享态先识别出的会话只负责阻止它转发到 P4，本机 Hook 必须完整放行。
    // 即使随后进入独占，也不能在同一物理按键中途改变 down/up 的吞键策略。
    if (flow && !(flow & POINTER_KEY_SWALLOW)) {
        swallow = false;
        return true;
    }
    if (!up) {
        // Hook 与 Raw Keyboard 并非在所有机器上都会同时到达。这里只登记 Hook 已见，
        // Raw 位必须由真实 Raw 报告设置；否则缺少 Raw 抬起沿时会留下永久锁存。
        constexpr uint8_t bits = POINTER_KEY_HOOK | POINTER_KEY_SWALLOW;
        const uint32_t previousState = addPointerFlow(action, virtualKey, bits);
        const uint8_t previous = pointerFlowBits(previousState);
        flow = static_cast<uint8_t>(previous | bits);
        if (!(previous & (POINTER_KEY_HOOK | POINTER_KEY_RAW)))
            consumePointerModifiers(hidModifiers);
        // Raw Input 极少数情况下可能先到；谁先锁存会话谁负责投递，另一条路径只去重。
        if (!(previous & (POINTER_KEY_HOOK | POINTER_KEY_RAW)) && s_pointerHotkeys.main)
            PostMessageW(s_pointerHotkeys.main, WM_HOTKEY, POINTER_HOTKEYS[action].messageId, 0);
    }
    swallow = (flow & POINTER_KEY_SWALLOW) != 0;
    if (up)
        removePointerFlow(action, virtualKey,
            POINTER_KEY_HOOK | POINTER_KEY_SWALLOW);
    return true;
}

bool handlePointerRaw(UINT virtualKey, bool up, uint8_t hidModifiers) {
    int action = latchedPointerAction(virtualKey);
    // 共享输入时也要识别并截住 Pointer 的专用键，避免同一按键继续转发到 P4；
    // 此时动作由 Pointer 的 RegisterHotKey 处理，只有独占态才由 Link 直投。
    if (action < 0 && !up)
        action = matchingPointerAction(virtualKey, pointerModifiers(hidModifiers));
    if (action < 0) return false;

    if (up) {
        // Raw 先抬起时保留 Hook 位和独占吞键策略，后到的 Hook up 仍会被吞；若 Hook
        // up 丢失，2 秒陈旧会话兜底。Raw-only 会话在这里一次清完。
        removePointerFlow(action, virtualKey, POINTER_KEY_RAW, true);
        return true;
    }

    const bool grabbed = g.grabbed.load(std::memory_order_relaxed);
    const uint8_t current = pointerFlowBits(
        s_pointerFlows[action].load(std::memory_order_acquire));
    const uint8_t bits = static_cast<uint8_t>(POINTER_KEY_RAW |
        (!current && grabbed ? POINTER_KEY_SWALLOW : 0));
    const uint32_t previousState = addPointerFlow(action, virtualKey, bits);
    const uint8_t previous = pointerFlowBits(previousState);
    const uint8_t flow = static_cast<uint8_t>(previous | bits);
    // 共享态由系统 RegisterHotKey 执行动作，但对应组合仍是 Pointer 专用输入：
    // Raw 首次精确命中时也要消费待决修饰键，不能只截主键而把 Ctrl/Alt 泄漏到 P4。
    if (!(previous & (POINTER_KEY_HOOK | POINTER_KEY_RAW)))
        consumePointerModifiers(hidModifiers);
    // Hook 正常时由 Hook 投递；Hook 被系统静默移除后 Raw Input 仍可完成动作。
    // 同一次按住产生的 Raw 自动重复已经带 RAW 位，不能再次触发切换动作。
    if ((flow & POINTER_KEY_SWALLOW) &&
            !(previous & (POINTER_KEY_HOOK | POINTER_KEY_RAW)) && s_pointerHotkeys.main)
        PostMessageW(s_pointerHotkeys.main, WM_HOTKEY, POINTER_HOTKEYS[action].messageId, 0);
    return true;
}

// 位移报告队列:直通档必须保留每个 Raw Input 报告的时间颗粒。旧实现只用三个原子累加,
// sender 只要因同步串口写/线程调度短暂落后,4 个 1ms 报告就会塌成 1 帧——总位移没丢,
// 观感却稳定退成约 250Hz。固定档按 Motion.atUs 分时间片聚合,不再把后台积压整批吃掉。
constexpr size_t MOTION_CAP = 256;   // 过载最多保留一小段历史,避免恢复后长时间回放陈旧输入
capture::Motion s_motion[MOTION_CAP]{};
size_t s_motionHead = 0, s_motionTail = 0, s_motionCount = 0;
std::mutex s_motionMx;

void pushMotion(int dx, int dy, int wheel, uint64_t atUs) {
    if (dx == 0 && dy == 0 && wheel == 0) return;
    std::lock_guard<std::mutex> lk(s_motionMx);
    if (s_motionCount > 0) {
        size_t last = (s_motionTail + MOTION_CAP - 1) % MOTION_CAP;
        if (atUs < s_motion[last].atUs) atUs = s_motion[last].atUs; // 两生产者交错时仍保持队列单调
    }
    if (s_motionCount >= MOTION_CAP) {
        // 极端过载只合并到最新一份:守住总位移,同时把待回放帧数封顶。
        size_t last = (s_motionTail + MOTION_CAP - 1) % MOTION_CAP;
        s_motion[last].dx += dx;
        s_motion[last].dy += dy;
        s_motion[last].wheel += wheel;
        s_motion[last].atUs = atUs;
        return;
    }
    s_motion[s_motionTail] = {dx, dy, wheel, atUs};
    s_motionTail = (s_motionTail + 1) % MOTION_CAP;
    s_motionCount++;
}

// MSG.time 是输入进入线程消息队列的时刻,即使后台线程 10ms 后才获调度,这批消息仍保留
// 原始毫秒顺序。扩展到 GetTickCount64 的当前 64 位周期；同一毫秒最多按 8K 鼠标的
// 125µs 间隔排序,让 2K/4K/8K 固定档也能逐拍消费而不是同毫秒全合并。
uint64_t rawMessageTimeUs() {
    uint32_t low = (uint32_t)GetMessageTime();
    uint64_t now = GetTickCount64();
    uint64_t ms = (now & ~0xFFFFFFFFull) | low;
    if (ms > now + 0x80000000ull) ms -= 0x100000000ull;
    else if (now > ms + 0x80000000ull) ms += 0x100000000ull;
    static uint32_t lastLow = 0;
    static unsigned ordinal = 0;
    if (low == lastLow) ordinal++;
    else { lastLow = low; ordinal = 0; }
    return ms * 1000ull + (uint64_t)(ordinal < 8 ? ordinal : 7) * 125ull;
}
// 物理位移镜像:monitor 回读用(只累积物理,不受屏蔽影响;netserver 原子取走)
std::atomic<long> s_monDx{0}, s_monDy{0}, s_monWheel{0};

// 键鼠状态:物理(p*)与网络(n*)两源分开存,对外一律给"合流后"快照。
// 物理由捕获线程改,网络由 netserver 线程改 → 全部走 s_stateMx 短临界区。
std::mutex s_stateMx;
uint8_t s_pBtn = 0, s_pMod = 0, s_pKeys[6] = {};
uint8_t s_pVk[6] = {};      // 与 s_pKeys 一一对应的虚拟键码,供 healPhysical() 回读系统真值
uint8_t s_observedMods = 0; // 物理真值；Pointer 前缀待决期间不会提前进入 s_pMod
constexpr size_t PENDING_MODIFIER_CAP = 8;
uint8_t s_pendingModifierDowns[PENDING_MODIFIER_CAP] = {};
size_t s_pendingModifierCount = 0;
uint8_t s_consumedPointerMods = 0; // 已实际命中 Pointer，保持隐藏直至对应物理 up
uint8_t s_nBtn = 0, s_nMod = 0, s_nKeys[6] = {};
// 屏蔽物理鼠标位图(kmboxNET MASK):bit0-4=键 bit5=X bit6=Y bit7=滚轮。只挡"物理→转发",
// 不挡 monitor 回读(第三方要读真实物理算补偿)。
std::atomic<uint8_t> s_physMask{0};
std::atomic<bool> s_uiMouseBlocked{false};

// 边沿队列
std::mutex s_edgeMx;
std::deque<capture::Edge> s_edges;

// 合流快照(调用者须持有 s_stateMx):物理(按屏蔽位过滤按键)| 网络
void buildCombinedLocked(uint8_t& btn, uint8_t& mod, uint8_t keys[6]) {
    uint8_t pm = s_physMask.load();
    uint8_t pbtn = s_pBtn & ~(pm & 0x1F);      // 屏蔽低 5 位 = 物理鼠标键
    btn = pbtn | s_nBtn;
    mod = s_pMod | s_nMod;
    memset(keys, 0, 6);
    int idx = 0;
    for (int i = 0; i < 6 && idx < 6; i++) if (s_pKeys[i]) keys[idx++] = s_pKeys[i];
    for (int i = 0; i < 6 && idx < 6; i++) {
        uint8_t u = s_nKeys[i]; if (!u) continue;
        bool dup = false; for (int j = 0; j < idx; j++) if (keys[j] == u) dup = true;
        if (!dup) keys[idx++] = u;
    }
}

struct KeyEdgeBatch {
    capture::Edge values[PENDING_MODIFIER_CAP + 1]{};
    size_t count = 0;
};

void appendKeyEdgeLocked(KeyEdgeBatch& edges) {
    if (edges.count >= PENDING_MODIFIER_CAP + 1) return;
    capture::Edge& edge = edges.values[edges.count++];
    edge.isKey = true;
    buildCombinedLocked(edge.btn, edge.mod, edge.keys);
}

bool removePendingModifierLocked(uint8_t bit) {
    for (size_t index = 0; index < s_pendingModifierCount; ++index) {
        if (s_pendingModifierDowns[index] != bit) continue;
        for (size_t tail = index + 1; tail < s_pendingModifierCount; ++tail)
            s_pendingModifierDowns[tail - 1] = s_pendingModifierDowns[tail];
        s_pendingModifierDowns[--s_pendingModifierCount] = 0;
        return true;
    }
    return false;
}

bool isPendingModifierLocked(uint8_t bit) {
    for (size_t index = 0; index < s_pendingModifierCount; ++index)
        if (s_pendingModifierDowns[index] == bit) return true;
    return false;
}

void addPendingModifierLocked(uint8_t bit) {
    for (size_t index = 0; index < s_pendingModifierCount; ++index)
        if (s_pendingModifierDowns[index] == bit) return;
    if (s_pendingModifierCount < PENDING_MODIFIER_CAP)
        s_pendingModifierDowns[s_pendingModifierCount++] = bit;
}

// Pointer 主键已经精确命中后才提交“消费”。尚未进入 s_pMod 的物理修饰键保持隐藏，
// 并从待决队列移除；已经送到 P4 的位不能半途吞掉其 up，否则会造成板端卡键。
void consumePointerModifiers(uint8_t hidModifiers) {
    std::lock_guard<std::mutex> lk(s_stateMx);
    const uint8_t newlyConsumed = static_cast<uint8_t>(hidModifiers & ~s_pMod);
    s_consumedPointerMods |= newlyConsumed;
    for (uint8_t bit = 1; bit; bit = static_cast<uint8_t>(bit << 1))
        if (newlyConsumed & bit) removePendingModifierLocked(bit);
}

// 调用者持有 s_stateMx。每个修饰键各生成一份快照，保证 Ctrl→Alt→普通键的板端
// 顺序与物理顺序一致；已经松开或已经转发的位不会重复补发。
void flushPendingModifiersLocked(KeyEdgeBatch& edges) {
    for (size_t index = 0; index < s_pendingModifierCount; ++index) {
        const uint8_t bit = s_pendingModifierDowns[index];
        if ((s_observedMods & bit) && !(s_pMod & bit)) {
            s_pMod |= bit;
            appendKeyEdgeLocked(edges);
        }
    }
    memset(s_pendingModifierDowns, 0, sizeof(s_pendingModifierDowns));
    s_pendingModifierCount = 0;
}

void publishKeyEdges(const KeyEdgeBatch& edges) {
    if (!edges.count) return;
    {
        std::lock_guard<std::mutex> lk(s_edgeMx);
        for (size_t index = 0; index < edges.count; ++index) {
            if (s_edges.size() >= 256) break;
            s_edges.push_back(edges.values[index]);
        }
    }
    SetEvent(capture::evtInput);
    capture::notifyMonitor();
    g.inEvents.fetch_add(static_cast<uint32_t>(edges.count), std::memory_order_relaxed);
}

// 推边沿帧:带合流后状态。调用者不持锁(内部自取 s_stateMx 建快照)。
void pushEdge(bool isKey) {
    capture::Edge e{};
    e.isKey = isKey;
    { std::lock_guard<std::mutex> lk(s_stateMx); buildCombinedLocked(e.btn, e.mod, e.keys); }
    std::lock_guard<std::mutex> lk(s_edgeMx);
    if (s_edges.size() < 256) s_edges.push_back(e);
    SetEvent(capture::evtInput);
}

// 物理按键增删(持 s_stateMx 调用)。vk 同步记录,healPhysical 靠它回读系统真值。
void keysAddLocked(uint8_t u, uint8_t vk) {
    for (int i = 0; i < 6; i++) if (s_pKeys[i] == u) return;
    for (int i = 0; i < 6; i++) if (s_pKeys[i] == 0) { s_pKeys[i] = u; s_pVk[i] = vk; return; }
    memmove(s_pKeys, s_pKeys + 1, 5);
    memmove(s_pVk,   s_pVk   + 1, 5);
    s_pKeys[5] = u; s_pVk[5] = vk;
}
void keysDelLocked(uint8_t u) {
    for (int i = 0; i < 6; i++) if (s_pKeys[i] == u) { s_pKeys[i] = 0; s_pVk[i] = 0; }
}

// ---- 独占实现:LL Hook 吞本机输入(鼠标/键盘捕获路径不同,见 capture.h 头注) ----
//
// 鼠标实测：LL Hook return 1 后 Raw Mouse 照常送达。
// 键盘不能照搬:用户实机确认后台时本机键盘已被 Hook 吞掉,但手机收不到对应按键;
// 因此 grabbed 时键盘由 Hook 投递 MSG_CAPTURE_HOOKKEY,Raw Keyboard 只在非独占时使用。
// 规则:
//   - 绑键中(bindWait)→ Hook 吞键盘并直接回报绑定结果;
//   - 收放热键 → Hook 全局检测,与焦点/最小化无关;
//   - grabbed → 键鼠在本程序窗口内外都吞;热键或 Ctrl+Alt+Q 负责逃生。
void postHotkeyEdge(bool up) {
    ULONGLONG now = GetTickCount64();
    if (up) {
        s_hotkeyDown = false;
        s_hotkeyLastEventMs = now;
        return;
    }
    ULONGLONG prev = s_hotkeyLastEventMs.exchange(now);
    bool wasDown = s_hotkeyDown.exchange(true);
    // Hook/Raw 同一物理沿会在数毫秒内各到一次,只触发一次。若抬起沿真的丢了,
    // 下一次相隔 2s 以上的按下可自愈；按住自动重复会持续刷新 prev,不会反复切换。
    if (!wasDown || (prev != 0 && now - prev > 2000)) PostMessage(g.hwnd, MSG_HOTKEY, 0, 0);
}

LRESULT CALLBACK kbHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        if (k->flags & LLKHF_INJECTED) return CallNextHookEx(nullptr, code, wp, lp);
        bool up = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
        bool e0 = (k->flags & LLKHF_EXTENDED) != 0;
        bool e1 = (k->vkCode == VK_PAUSE);

        // Hook 内独立跟踪修饰键。独占时系统按键表不会更新,应急解锁不能依赖
        // GetAsyncKeyState;同时必须在用户把 Q 绑成收放热键时仍优先保证逃生。
        uint8_t modBit = 0;
        (void)scanToHid((uint16_t)k->scanCode, e0, e1, &modBit);
        if (modBit) {
            if (up) s_hookMods &= ~modBit;
            else    s_hookMods |= modBit;
        }
        if (!up && (k->vkCode == 'Q' || k->vkCode == 'q') &&
            (s_hookMods & 0x11) && (s_hookMods & 0x44)) {
            PostMessage(g.hwnd, MSG_PANIC, 0, 0);
            return 1;
        }

        // Pointer 发布当前四组绑定。独占时 Link 精确匹配修饰键并直投动作，随后吞掉
        // 完整 down/up；非独占继续交给 Pointer 自己的 RegisterHotKey。
        bool swallowPointerKey = false;
        if (handlePointerHook(k->vkCode, up, s_hookMods, swallowPointerKey))
            return swallowPointerKey ? 1 : CallNextHookEx(nullptr, code, wp, lp);

        // 绑键和收放热键由全局 Hook 直接检测,不再依赖窗口焦点/后台 Raw Input。
        if (g.bindWait) {
            if (!up && k->vkCode != 0 && k->vkCode != 0xFF)
                PostMessage(g.hwnd, MSG_BINDDONE, (WPARAM)k->vkCode, 0);
            else if (up && s_captureWnd) {
                // 绑键开始前可能已有键处于按下态；抬起沿仍要进入状态机，
                // 否则该键会被后续全状态刷新永久重发到板端。
                LPARAM packed = (LPARAM)((uint32_t)k->scanCode |
                                          (e0 ? 1u << 16 : 0) |
                                          (e1 ? 1u << 17 : 0) |
                                          (1u << 18));
                PostMessageW(s_captureWnd, MSG_CAPTURE_HOOKKEY,
                             (WPARAM)k->vkCode, packed);
            }
            return 1;
        }
        if ((UINT)k->vkCode == g.hotkeyVk.load()) {
            postHotkeyEdge(up);
            return 1;   // 收放热键始终保留,不漏给游戏也不映射上板
        }

        if (g.grabbed) {
            // 键盘与鼠标不同:实机反馈表明后台时 LL Hook 吞入后 Raw Keyboard 可能不再
            // 到达。独占态由 Hook 自己投递扫描码到捕获窗,再更新边沿/全状态。
            LPARAM packed = (LPARAM)((uint32_t)k->scanCode |
                                      (e0 ? 1u << 16 : 0) |
                                      (e1 ? 1u << 17 : 0) |
                                      (up ? 1u << 18 : 0));
            if (s_captureWnd) PostMessageW(s_captureWnd, MSG_CAPTURE_HOOKKEY,
                                           (WPARAM)k->vkCode, packed);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK msHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* m = (MSLLHOOKSTRUCT*)lp;
        if (m->flags & LLMHF_INJECTED) return CallNextHookEx(nullptr, code, wp, lp);
        // 遮罩语义 = 窗口内外一律拦截。要操作本程序,先按收放热键释放;
        // 忘记热键仍可用固定 Ctrl+Alt+Q 应急解锁。
        if (g.grabbed) return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// ---- Raw Input ----

void onRawMouse(const RAWMOUSE& m, uint64_t eventUs) {
    if (m.usFlags & MOUSE_MOVE_ABSOLUTE) return;   // 绝对坐标设备(数位板等)不转发
    // 共享模式操作任务栏控制岛时，不把这次移动/点击同时送到手机。真正独占时本机
    // 光标被冻结，必须继续消费 Raw Input，因此 grabbed 优先于 UI 门禁。
    if (!g.grabbed.load(std::memory_order_relaxed) &&
        s_uiMouseBlocked.load(std::memory_order_relaxed)) return;
    USHORT f = m.usButtonFlags;
    int wheel = (f & RI_MOUSE_WHEEL) ? (SHORT)m.usButtonData / WHEEL_DELTA : 0;

    // monitor 镜像:物理增量无条件累积(不受屏蔽影响,第三方读真实物理算补偿)
    if (m.lLastX) s_monDx += m.lLastX;
    if (m.lLastY) s_monDy += m.lLastY;
    if (wheel)    s_monWheel += wheel;

    // 转发位移:屏蔽位过滤(X/Y/滚轮各自可屏蔽)
    uint8_t pm = s_physMask.load();
    int fwdDx = (pm & 0x20) ? 0 : (int)m.lLastX;
    int fwdDy = (pm & 0x40) ? 0 : (int)m.lLastY;
    int fwdWh = (pm & 0x80) ? 0 : wheel;
    pushMotion(fwdDx, fwdDy, fwdWh, eventUs);

    bool edge = false;
    { std::lock_guard<std::mutex> lk(s_stateMx);
      if (f & RI_MOUSE_LEFT_BUTTON_DOWN)   { s_pBtn |=  0x01; edge = true; }
      if (f & RI_MOUSE_LEFT_BUTTON_UP)     { s_pBtn &= ~0x01; edge = true; }
      if (f & RI_MOUSE_RIGHT_BUTTON_DOWN)  { s_pBtn |=  0x02; edge = true; }
      if (f & RI_MOUSE_RIGHT_BUTTON_UP)    { s_pBtn &= ~0x02; edge = true; }
      if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) { s_pBtn |=  0x04; edge = true; }
      if (f & RI_MOUSE_MIDDLE_BUTTON_UP)   { s_pBtn &= ~0x04; edge = true; }
      if (f & RI_MOUSE_BUTTON_4_DOWN)      { s_pBtn |=  0x08; edge = true; }
      if (f & RI_MOUSE_BUTTON_4_UP)        { s_pBtn &= ~0x08; edge = true; }
      if (f & RI_MOUSE_BUTTON_5_DOWN)      { s_pBtn |=  0x10; edge = true; }
      if (f & RI_MOUSE_BUTTON_5_UP)        { s_pBtn &= ~0x10; edge = true; }
    }
    if (edge || m.lLastX || m.lLastY || wheel) capture::notifyMonitor();
    if (edge) pushEdge(false);   // pushEdge 用合流快照,屏蔽位已在 buildCombinedLocked 生效
    g.inEvents++;
    SetEvent(capture::evtInput);
}

void onKeyboardEvent(UINT vk, uint16_t makeCode, bool e0, bool e1, bool up) {
    // 应急解锁 Ctrl+Alt+Q(固定,不可改):独占把键鼠全吞、绑的热键又按不出来(键盘没有
    // ScrollLock / 绑错了 / 忘了绑的是哪个)时的最后出口。修饰键用物理观察态判断，
    // 不用 GetAsyncKeyState —— 独占中 LL 钩子 return 1 会让系统按键表压根不更新。
    if (!up && (vk == 'Q' || vk == 'q')) {
        uint8_t curMod;
        { std::lock_guard<std::mutex> lk(s_stateMx); curMod = s_observedMods; }
        if ((curMod & 0x11) && (curMod & 0x44)) {   // 任一 Ctrl + 任一 Alt
            PostMessage(g.hwnd, MSG_PANIC, 0, 0);
            return;
        }
    }
    uint8_t modBit = 0;
    uint8_t usage = scanToHid(makeCode, e0, e1, &modBit);
    if (!usage && !modBit) return;

    // Pointer 发布组合后先暂存所有“可能成为组合前缀”的修饰键。若随后命中 Pointer，
    // 主键由上层截住，修饰键 down/up 都不会到 P4；若随后是普通键，则在同一锁内按
    // 原始顺序补出修饰键快照，再补普通键，Hook/Raw 双路也只会改变一次观察态。
    const bool pendingCandidate = modBit && !up &&
        (pointerModifierPrefixMask() & pointerModifiers(modBit));
    KeyEdgeBatch edges;
    {
        std::lock_guard<std::mutex> lk(s_stateMx);
        if (modBit) {
            const bool wasObserved = (s_observedMods & modBit) != 0;
            if ((up && !wasObserved) || (!up && wasObserved))
                return; // Hook 与 Raw 的同一物理沿：后到者直接去重

            if (!up) {
                s_observedMods |= modBit;
                // 主键可能先由 Hook 命中，随后才处理较早 PostMessage 的 modifier down。
                // consumed 位让这种倒序仍保持隐藏，不会重新进入待决队列。
                if (s_consumedPointerMods & modBit) {
                    removePendingModifierLocked(modBit);
                    return;
                }
                if (pendingCandidate && !(s_pMod & modBit)) {
                    addPendingModifierLocked(modBit);
                    return;
                }
                // 当前修饰键本身需要送出时，先补齐更早到达的待决修饰键。
                flushPendingModifiersLocked(edges);
                if (!(s_pMod & modBit)) {
                    s_pMod |= modBit;
                    appendKeyEdgeLocked(edges);
                }
            } else {
                if (s_consumedPointerMods & modBit) {
                    s_consumedPointerMods &= ~modBit;
                    s_observedMods &= ~modBit;
                    removePendingModifierLocked(modBit);
                    return;
                }
                // 未命中 Pointer 的待决修饰键不能丢：先按物理顺序补 down，再补本次 up。
                // 若它本来已转发，也先补齐期间更晚按下的其它待决修饰键。
                if (isPendingModifierLocked(modBit) || (s_pMod & modBit))
                    flushPendingModifiersLocked(edges);
                s_observedMods &= ~modBit;
                if (s_pMod & modBit) {
                    s_pMod &= ~modBit;
                    appendKeyEdgeLocked(edges);
                }
            }
        } else {
            uint8_t oldKeys[6];
            memcpy(oldKeys, s_pKeys, sizeof(oldKeys));
            if (!up) flushPendingModifiersLocked(edges);
            if (up) keysDelLocked(usage);
            else    keysAddLocked(usage, static_cast<uint8_t>(vk));
            if (memcmp(oldKeys, s_pKeys, sizeof(oldKeys)) != 0)
                appendKeyEdgeLocked(edges);
        }
    }
    publishKeyEdges(edges);
}

void onRawKeyboard(const RAWKEYBOARD& k) {
    bool up = (k.Flags & RI_KEY_BREAK) != 0;
    uint8_t modifiers = 0;
    {
        std::lock_guard<std::mutex> lock(s_stateMx);
        modifiers = s_observedMods;
    }
    // Raw Input 也必须先守住固定逃生键；Hook 被系统静默移除时不能让用户配置的
    // Pointer 动作抢走 Ctrl+Alt+Q。
    if (!up && (k.VKey == 'Q' || k.VKey == 'q') &&
            (modifiers & 0x11) && (modifiers & 0x44)) {
        PostMessage(g.hwnd, MSG_PANIC, 0, 0);
        return;
    }
    if (handlePointerRaw(k.VKey, up, modifiers)) return;
    // 绑键 / 热键检测走 Raw Input:INPUTSINK 前台后台都收到,焦点无关。
    // 与 Hook 双路并行而不是仅在 Hook 安装失败时后备:Hook 可能超时后被系统静默移除,
    // 但旧的句柄状态并不会自动变 false。postHotkeyEdge 自带双源去重。
    if (g.bindWait) {
        if (!up) {
            if (k.VKey != 0 && k.VKey != 0xFF)
                PostMessage(g.hwnd, MSG_BINDDONE, (WPARAM)k.VKey, 0);
            return;
        }
        // ⚠️ 抬起沿必须继续往下走状态维护,绝不能在这里 return:
        //   绑键期间吞掉 up,会把此前按住的键永久留在 s_pKeys 里,再被 100ms 全状态刷新帧
        //   一路重发 → 板端触点永久卡住、"不动键鼠也一直触发按键"。用户反馈的就是这条路。
    }
    if (k.VKey == g.hotkeyVk.load()) {
        postHotkeyEdge(up);
        return;                                    // 热键保留,不映射上板
    }
    // 独占态 Hook 也会投递同一沿,但 onKeyboardEvent 以状态 diff 去重；Raw 仍然进入,
    // 防 Hook 被系统静默移除后“本机已吞/手机却收不到”的失效模式。
    onKeyboardEvent(k.VKey, k.MakeCode, (k.Flags & RI_KEY_E0) != 0,
                    (k.Flags & RI_KEY_E1) != 0, up);
}

// 高频鼠标在本进程位于后台时，多个 WM_INPUT 可能已经积压在线程队列中。
// 只对当前 lParam 调 GetRawInputData 会让每个报告都额外走一轮消息分派；前台时不明显，
// 后台调度稍有延后便会形成约 10ms 的批次。这里按微软文档建议组合使用：先读取当前
// HRAWINPUT，再用 GetRawInputBuffer 一次排空其后的报告，并保留每个 RAWMOUSE 结构。
struct PendingRawInput {
    HANDLE device = nullptr;
    DWORD type = 0;
    RAWMOUSE mouse{};
    RAWKEYBOARD keyboard{};
};

bool appendPendingRaw(const RAWINPUT* ri, std::vector<PendingRawInput>& pending) {
    if (!ri || ri->header.hDevice == nullptr) return false;  // SendInput 注入不回环转发
    PendingRawInput p{};
    p.device = ri->header.hDevice;
    p.type = ri->header.dwType;
    if (p.type == RIM_TYPEMOUSE) p.mouse = ri->data.mouse;
    else if (p.type == RIM_TYPEKEYBOARD) p.keyboard = ri->data.keyboard;
    else return false;                                      // 本工具未注册通用 HID
    pending.push_back(p);
    return true;
}

void dispatchRawInputBatch(HRAWINPUT current, uint64_t currentMessageUs) {
    static thread_local std::vector<BYTE> currentBuf(512);
    static thread_local std::vector<PendingRawInput> pending;
    pending.clear();
    if (pending.capacity() < 512) pending.reserve(512);
    bool currentIsMouse = false;

    // 当前 WM_INPUT 已经被 GetMessage 取出，不在 GetRawInputBuffer 的返回集合中，必须先读。
    UINT currentSize = 0;
    if (GetRawInputData(current, RID_INPUT, nullptr, &currentSize,
                        sizeof(RAWINPUTHEADER)) == 0 && currentSize > 0 &&
        currentSize <= 64 * 1024) {
        if (currentBuf.size() < currentSize) currentBuf.resize(currentSize);
        UINT readSize = currentSize;
        if (GetRawInputData(current, RID_INPUT, currentBuf.data(), &readSize,
                            sizeof(RAWINPUTHEADER)) == currentSize) {
            size_t before = pending.size();
            appendPendingRaw(reinterpret_cast<const RAWINPUT*>(currentBuf.data()), pending);
            currentIsMouse = pending.size() != before && pending.back().type == RIM_TYPEMOUSE;
        }
    }

    // RAWINPUT 块要求按指针对齐；64KiB 可容纳数百个鼠标/键盘报告。循环直到线程队列排空，
    // 避免一次调度间隙超过缓冲区容量时仍留下尾批。
    static thread_local std::vector<BYTE> rawBuffer(64 * 1024);
    for (unsigned pass = 0; pass < 8; ++pass) {
        UINT bufferBytes = static_cast<UINT>(rawBuffer.size());
        UINT count = GetRawInputBuffer(reinterpret_cast<PRAWINPUT>(rawBuffer.data()), &bufferBytes,
                                       sizeof(RAWINPUTHEADER));
        if (count == static_cast<UINT>(-1) || count == 0) break;

        const BYTE* end = rawBuffer.data() + rawBuffer.size();
        PRAWINPUT ri = reinterpret_cast<PRAWINPUT>(rawBuffer.data());
        for (UINT i = 0; i < count; ++i) {
            const BYTE* begin = reinterpret_cast<const BYTE*>(ri);
            if (begin + sizeof(RAWINPUTHEADER) > end ||
                ri->header.dwSize < sizeof(RAWINPUTHEADER) ||
                begin + ri->header.dwSize > end) break;
            appendPendingRaw(ri, pending);
            uintptr_t next = reinterpret_cast<uintptr_t>(begin + ri->header.dwSize);
            next = (next + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1);
            ri = reinterpret_cast<PRAWINPUT>(next);
        }
    }

    if (pending.empty()) return;

    size_t mouseCount = 0;
    for (const auto& p : pending) if (p.type == RIM_TYPEMOUSE) ++mouseCount;

    // GetRawInputBuffer 不附带逐报告 MSG.time。用“当前消息入队时刻 → 本次排空时刻”均匀恢复
    // 批内时间线；1K 鼠标后台积压 10 个报告时会得到约 1ms 间隔，而不是十个同拍合并。
    uint64_t mouseUs = s_lastRawMouseTimelineUs.load(std::memory_order_relaxed);
    uint64_t gapUs = 1000;
    uint64_t batchEndUs = GetTickCount64() * 1000ull;
    size_t followingMouse = mouseCount - (currentIsMouse ? 1u : 0u);

    if (currentIsMouse) {
        mouseUs = std::max(mouseUs, currentMessageUs);
        if (followingMouse > 0) {
            uint64_t spanUs = batchEndUs > mouseUs ? batchEndUs - mouseUs : 0;
            uint64_t inferredUs = spanUs / followingMouse;
            if (inferredUs >= 125 && inferredUs <= 8000) {
                gapUs = inferredUs;
            } else {
                // 极长停顿不回放陈旧时间线；同一毫秒内的 8K 批次按 125us 排列。
                gapUs = inferredUs < 125 ? 125 : 1000;
                mouseUs = batchEndUs > gapUs * followingMouse
                    ? batchEndUs - gapUs * followingMouse : 0;
            }
        }
    } else if (mouseCount > 0) {
        uint64_t spanUs = batchEndUs > mouseUs ? batchEndUs - mouseUs : 0;
        uint64_t inferredUs = spanUs / mouseCount;
        if (mouseUs != 0 && inferredUs >= 125 && inferredUs <= 8000) gapUs = inferredUs;
        else gapUs = inferredUs < 125 ? 125 : 1000;
        mouseUs = batchEndUs > gapUs * mouseCount ? batchEndUs - gapUs * mouseCount : 0;
    }

    bool firstMouse = true;
    for (const auto& p : pending) {
        if (p.type == RIM_TYPEMOUSE) {
            if (!(currentIsMouse && firstMouse)) mouseUs += gapUs;
            onRawMouse(p.mouse, mouseUs);
            firstMouse = false;
        } else {
            onRawKeyboard(p.keyboard);
        }
    }
    if (mouseCount > 0) s_lastRawMouseTimelineUs.store(mouseUs, std::memory_order_relaxed);
}

LRESULT CALLBACK capWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == MSG_CAPTURE_HOOKKEY) {
        uint32_t p = (uint32_t)lp;
        onKeyboardEvent((UINT)wp, (uint16_t)(p & 0xFFFF),
                        (p & (1u << 16)) != 0, (p & (1u << 17)) != 0,
                        (p & (1u << 18)) != 0);
        return 0;
    }
    if (msg == WM_INPUT) {
        // 前台 RIM_INPUT 处理后必须调用 DefWindowProc 让系统清理 HRAWINPUT；后台
        // RIM_INPUTSINK 无此要求。旧代码一律 return 0,正好只破坏前台输入路径。
        bool needCleanup = GET_RAWINPUT_CODE_WPARAM(wp) == RIM_INPUT;
        uint64_t messageUs = rawMessageTimeUs();
        dispatchRawInputBatch((HRAWINPUT)lp, messageUs);
        return needCleanup ? DefWindowProcW(h, msg, wp, lp) : 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void threadMain() {
    s_tid = GetCurrentThreadId();
    // Raw Input 与两个 LL Hook 共用本消息线程。游戏成为前台后若此线程被 EcoQoS/后台调度
    // 压住,实机表现为约 10ms 才处理一批鼠标报告。此线程每次只做入队/状态 diff,随后立即
    // 回到 GetMessage 阻塞,可安全与 sender 同用 TIME_CRITICAL(15)。
    auto perf = performance::EnableCurrentThreadHighPerformance(THREAD_PRIORITY_TIME_CRITICAL);
    if (!perf.priority) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        g.log(L"输入捕获实时优先级提升失败(" + std::to_wstring(perf.priorityError) +
              L"),已回退高优先级;后台高负载可能出现报告成簇");
    }
    if (!perf.executionSpeed) {
        g.log(L"输入捕获线程 HighQoS 设置失败(" + std::to_wstring(perf.powerError) +
              L"),请查看后台性能说明");
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = capWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"QLinkCapture";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    s_captureWnd = wnd;
    if (!wnd) g.log(L"输入捕获消息窗创建失败,键鼠转发不可用");

    RAWINPUTDEVICE rid[2]{};
    rid[0] = {0x01, 0x02, RIDEV_INPUTSINK, wnd};                     // 鼠标
    rid[1] = {0x01, 0x06, RIDEV_INPUTSINK, wnd};                     // 键盘
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE)))
        g.log(L"Raw Input 注册失败,请重启程序或检查系统输入设备");

    // LL 钩子装在本线程(有消息泵);未独占/未绑键时钩子体一行判断直通,开销可忽略
    s_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, kbHook, wc.hInstance, 0);
    s_msHook = SetWindowsHookExW(WH_MOUSE_LL, msHook, wc.hInstance, 0);
    if (!s_kbHook) g.log(L"键盘全局 Hook 安装失败,独占键盘/全局热键不可用");
    if (!s_msHook) g.log(L"鼠标全局 Hook 安装失败,独占鼠标不可用");

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (s_kbHook) { UnhookWindowsHookEx(s_kbHook); s_kbHook = nullptr; }
    if (s_msHook) { UnhookWindowsHookEx(s_msHook); s_msHook = nullptr; }
    DestroyWindow(wnd);
    s_captureWnd = nullptr;
    s_tid = 0;
}

}  // namespace

namespace capture {

HANDLE evtInput = nullptr;

void start() {
    evtInput = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s_thread = std::thread(threadMain);
}

void stop() {
    if (s_tid) PostThreadMessageW(s_tid, WM_QUIT, 0, 0);
    if (s_thread.joinable()) s_thread.join();
    if (evtInput) { CloseHandle(evtInput); evtInput = nullptr; }
}

bool popEdge(Edge& e) {
    std::lock_guard<std::mutex> lk(s_edgeMx);
    if (s_edges.empty()) return false;
    e = s_edges.front();
    s_edges.pop_front();
    return true;
}

bool popMotion(Motion& m) {
    std::lock_guard<std::mutex> lk(s_motionMx);
    if (s_motionCount == 0) return false;
    m = s_motion[s_motionHead];
    s_motionHead = (s_motionHead + 1) % MOTION_CAP;
    s_motionCount--;
    return true;
}

bool peekMotion(Motion& m) {
    std::lock_guard<std::mutex> lk(s_motionMx);
    if (s_motionCount == 0) return false;
    m = s_motion[s_motionHead];
    return true;
}

void fetchMotionBefore(uint64_t cutoffUs, int& dx, int& dy, int& wheel) {
    // 固定档只消费当前虚拟时间片。后台积压的 10 份 1ms 报告会在之后 10 个发送拍
    // 逐拍重放,而不是本拍全部聚合成一次大跳；同一时间片内的高回报率报告仍正常求和。
    int64_t sx = 0, sy = 0, sw = 0;
    std::lock_guard<std::mutex> lk(s_motionMx);
    while (s_motionCount > 0 && s_motion[s_motionHead].atUs < cutoffUs) {
        const Motion& m = s_motion[s_motionHead];
        sx += m.dx; sy += m.dy; sw += m.wheel;
        s_motionHead = (s_motionHead + 1) % MOTION_CAP;
        s_motionCount--;
    }
    if (s_motionCount == 0) s_motionTail = s_motionHead;
    dx = (int)std::clamp<int64_t>(sx, INT_MIN, INT_MAX);
    dy = (int)std::clamp<int64_t>(sy, INT_MIN, INT_MAX);
    wheel = (int)std::clamp<int64_t>(sw, INT_MIN, INT_MAX);
}

uint8_t btnState() {
    uint8_t b, mod, keys[6];
    std::lock_guard<std::mutex> lk(s_stateMx);
    buildCombinedLocked(b, mod, keys);
    return b;
}

void keyState(uint8_t& mod, uint8_t keys[6]) {
    uint8_t b;
    std::lock_guard<std::mutex> lk(s_stateMx);
    buildCombinedLocked(b, mod, keys);
}

// 物理状态自愈:拿系统的按键真值回读一遍,把"我们以为还按着、其实早松了"的键清掉。
//   残留成因不止一种:安全桌面/UAC/锁屏/切换用户期间 Raw Input 停投递而丢了抬起沿、
//   个别键(PAUSE、部分多媒体键)make/break 扫描码不对称、USB 重枚举。任何一次残留都会被
//   100ms 全状态刷新帧一路重发 → 板端触点永久卡住,所以必须有兜底而不是逐个堵成因。
//   ⚠️ 只在【非独占吞入】时可信:LL 钩子 return 1 时系统按键表根本不更新,回读全是"松开",
//   独占中调用会把真按着的键误清 —— 调用方(sender)必须先判 !g.grabbed。
bool healPhysical() {
    static const struct { uint8_t bit; int vk; } kMods[] = {
        {0x01, VK_LCONTROL}, {0x02, VK_LSHIFT}, {0x04, VK_LMENU}, {0x08, VK_LWIN},
        {0x10, VK_RCONTROL}, {0x20, VK_RSHIFT}, {0x40, VK_RMENU}, {0x80, VK_RWIN},
    };
    static const struct { uint8_t bit; int vk; } kBtns[] = {
        {0x01, VK_LBUTTON}, {0x02, VK_RBUTTON}, {0x04, VK_MBUTTON},
        {0x08, VK_XBUTTON1}, {0x10, VK_XBUTTON2},
    };
    bool changed = false;
    std::lock_guard<std::mutex> lk(s_stateMx);
    for (const auto& m : kMods) {
        if (!((s_observedMods | s_pMod | s_consumedPointerMods) & m.bit) ||
                (GetAsyncKeyState(m.vk) & 0x8000))
            continue;
        s_observedMods &= ~m.bit;
        s_consumedPointerMods &= ~m.bit;
        removePendingModifierLocked(m.bit);
        if (s_pMod & m.bit) { s_pMod &= ~m.bit; changed = true; }
    }
    for (const auto& b : kBtns)
        if ((s_pBtn & b.bit) && !(GetAsyncKeyState(b.vk) & 0x8000)) { s_pBtn &= ~b.bit; changed = true; }
    for (int i = 0; i < 6; i++)
        if (s_pKeys[i] && s_pVk[i] && !(GetAsyncKeyState(s_pVk[i]) & 0x8000)) {
            s_pKeys[i] = 0; s_pVk[i] = 0; changed = true;
        }
    return changed;
}

void resetState() {
    s_lastRawMouseTimelineUs.store(0, std::memory_order_relaxed);
    { std::lock_guard<std::mutex> lk(s_stateMx);
      s_pBtn = s_pMod = 0; memset(s_pKeys, 0, 6); memset(s_pVk, 0, 6);
      s_observedMods = 0;
      s_consumedPointerMods = 0;
      memset(s_pendingModifierDowns, 0, sizeof(s_pendingModifierDowns));
      s_pendingModifierCount = 0;
      s_nBtn = s_nMod = 0; memset(s_nKeys, 0, 6);
    }
    { std::lock_guard<std::mutex> lk(s_motionMx);
      s_motionHead = s_motionTail = s_motionCount = 0;
    }
    std::lock_guard<std::mutex> lk(s_edgeMx);
    s_edges.clear();
}

void resetHotkeyLatch() {
    s_hotkeyDown = false;
    s_hotkeyLastEventMs = 0;
    for (size_t index = 0; index < POINTER_HOTKEY_COUNT; ++index)
        clearPointerFlow(index);
}

// ── kmboxNET(netserver)注入接口:网络源与物理源合流 ──

void injectNetMouse(uint8_t btn, int dx, int dy, int wheel, bool absButtons) {
    pushMotion(dx, dy, wheel, GetTickCount64() * 1000ull);
    bool edge;
    { std::lock_guard<std::mutex> lk(s_stateMx);
      uint8_t old = s_nBtn;
      s_nBtn = absButtons ? btn : (s_nBtn | btn);   // MOUSE_MOVE 带全量按钮位=绝对;单键命令=或入
      edge = (s_nBtn != old);
    }
    if (edge) pushEdge(false);
    SetEvent(evtInput);
}

void injectNetKey(uint8_t mod, const uint8_t keys[6]) {
    bool changed;
    { std::lock_guard<std::mutex> lk(s_stateMx);
      uint8_t oldMod = s_nMod; uint8_t oldK[6]; memcpy(oldK, s_nKeys, 6);
      s_nMod = mod; memcpy(s_nKeys, keys, 6);
      changed = (s_nMod != oldMod || memcmp(oldK, s_nKeys, 6) != 0);
    }
    if (changed) pushEdge(true);
    SetEvent(evtInput);
}

void setPhysMask(uint8_t mask) { s_physMask = mask; }
void setUiMouseBlocked(bool blocked) { s_uiMouseBlocked.store(blocked, std::memory_order_relaxed); }

void fetchPhysMotion(int& dx, int& dy, int& wheel) {
    dx = (int)s_monDx.exchange(0);
    dy = (int)s_monDy.exchange(0);
    wheel = (int)s_monWheel.exchange(0);
}

void physState(uint8_t& btn, uint8_t& mod, uint8_t keys[6]) {
    std::lock_guard<std::mutex> lk(s_stateMx);
    btn = s_pBtn; mod = s_pMod; memcpy(keys, s_pKeys, 6);
}

// monitor 唤醒回调:netserver 起服后注册,物理输入到达即被 onRaw* 调用
static std::atomic<void (*)()> s_monCb{nullptr};   // 跨线程(UI 起停服 vs 捕获线程读)→ 原子
void setMonitorCallback(void (*cb)()) { s_monCb = cb; }
void notifyMonitor() { auto cb = s_monCb.load(); if (cb) cb(); }

}  // namespace capture
