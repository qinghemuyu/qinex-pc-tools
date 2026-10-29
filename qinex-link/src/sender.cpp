#include "sender.h"
#include "protocol.h"
#include "app.h"
#include "link.h"
#include "capture.h"
#include "performance.h"
#include <thread>
#include <algorithm>
#pragma comment(lib, "winmm.lib")
#include <timeapi.h>

namespace {

std::thread s_thread;
std::atomic<bool> s_stop{false};
std::mutex s_cycleMx;   // 串行化发送循环与 UI 的同步全抬起,保证释放帧最后上线

// 报文编码与公开协议共用纯函数，发送线程只负责当前链路交付。
void sendMouse(uint8_t btn, int dx, int dy, int wh, uint16_t gapUs = 0) {
    if (g.mouseV2.load()) {
        const auto frame = link_protocol::TimedMouse(btn, dx, dy, wh, gapUs);
        link::write(frame.data(), static_cast<int>(frame.size()));
    } else {
        const auto frame = link_protocol::Mouse(btn, dx, dy, wh);
        link::write(frame.data(), static_cast<int>(frame.size()));
    }
}

// 把捕获侧/固定切片的原始时间线压成 F8 的 16 位间隔。长空闲用 0 表示重新锚定；
// 连续输入保留真实间隔，使串口驱动即使晚 10ms 批量交付，P4 仍能按原颗粒逐拍重放。
uint16_t motionGapUs(uint64_t stampUs, uint64_t& lastStampUs) {
    uint16_t out = 0;
    if (lastStampUs && stampUs > lastStampUs) {
        uint64_t gap = stampUs - lastStampUs;
        if (gap <= 31875) out = (uint16_t)gap;
    }
    lastStampUs = stampUs;
    return out;
}

void sendKeys(uint8_t mod, const uint8_t keys[6]) {
    const auto frame = link_protocol::Keyboard(mod, keys);
    link::write(frame.data(), static_cast<int>(frame.size()));
}

void sendPing() {
    const auto frame = link_protocol::Ping(nowUs());
    link::write(frame.data(), static_cast<int>(frame.size()));
}

// 精确等待到绝对时刻(µs):>2ms 用 Sleep(1) 粗等,尾段自旋 —— 8K 档 125µs 节拍必需
void preciseWaitUntil(uint64_t target) {
    for (;;) {
        uint64_t now = nowUs();
        if (now >= target) return;
        if (target - now > 2000) Sleep(1);
        else YieldProcessor();
    }
}

void loop() {
    auto perf = performance::EnableCurrentThreadHighPerformance(THREAD_PRIORITY_TIME_CRITICAL);
    if (!perf.priority) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        g.log(L"帧发送实时优先级提升失败(" + std::to_wstring(perf.priorityError) +
              L"),已回退高优先级");
    }
    if (!perf.executionSpeed) {
        g.log(L"帧发送线程 HighQoS 设置失败(" + std::to_wstring(perf.powerError) +
              L"),请查看后台性能说明");
    }
    timeBeginPeriod(1);
    uint64_t nextTick = nowUs();
    uint64_t lastRefresh = 0, lastPing = 0, lastHeal = 0;
    uint64_t motionSliceEndUs = 0; // 固定档输入回放的虚拟时间片末端(Motion.atUs 时基)
    int motionSliceTier = -1;
    uint64_t lastMotionStampUs = 0; // 只跟位移时间线走；鼠标键边沿/心跳不扰动报告间隔
    bool wasFwd = false;
    uint32_t truncSeen = 0;        // 已上报过的写截断数
    uint64_t lastTruncLog = 0;     // 限流:截断一旦发生往往连片,别刷屏

    while (!s_stop) {
        if (!link::txReady()) {
            if (wasFwd) { wasFwd = false; capture::resetState(); }
            motionSliceEndUs = 0; motionSliceTier = -1;
            lastMotionStampUs = 0;
            Sleep(50);
            nextTick = nowUs();
            continue;
        }

        // 等待阶段绝不能持 s_cycleMx。发送线程是 TIME_CRITICAL；旧代码持锁等待下一拍，
        // UI 的“停转发/断开”在 releaseAll() 里可能长期抢不到锁，表现为第二次热键无响应、
        // 点击断开整窗卡死。先按当前状态等待，真正组帧/写串口时才进入短临界区。
        bool waitFwd = g.forwarding.load();
        int tier = g.tier;
        if (waitFwd && tier == 0) {
            // 直通:每来一个事件立刻发
            WaitForSingleObject(capture::evtInput, 1);
        } else {
            uint64_t interval = waitFwd ? (1000000ull / (uint64_t)(tier > 0 ? tier : 1000))
                                        : 20000;   // 非转发只维持心跳节拍
            nextTick += interval;
            uint64_t now = nowUs();
            if (nextTick < now) nextTick = now;   // 落后就贴现在,不追帧
            preciseWaitUntil(nextTick);
        }

        std::lock_guard<std::mutex> cycleLock(s_cycleMx);
        // 独占"放行"(grabbed=false)只是把本机输入还给用户,是否继续上板由 forwarding 决定
        bool fwd = g.forwarding.load();
        if (wasFwd != fwd) {
            // 转发开/停瞬间都发一轮全抬起:停 = 防板端卡键;开 = 清掉上次会话
            // (异常退出/丢帧)遗留在板端的卡住触点
            uint8_t zero[6] = {};
            sendKeys(0, zero);
            sendMouse(0, 0, 0, 0);
            capture::resetState();
            motionSliceEndUs = 0; motionSliceTier = -1;
            lastMotionStampUs = 0;
            lastRefresh = nowUs();
        }
        wasFwd = fwd;

        if (fwd) {
            // 1) 边沿帧按序 flush(位移 0,只带状态)——按下/抬起沿永不丢
            capture::Edge e;
            while (capture::popEdge(e)) {
                if (e.isKey) sendKeys(e.mod, e.keys);
                else         sendMouse(e.btn, 0, 0, 0);
                g.outFrames++;
            }
            // 2) 位移帧。直通档保留每个原始鼠标报告,不能因 evtInput 的自动复位信号
            // 合并或同步串口写短暂落后而压成一帧；固定档才按发送周期主动聚合。
            if (tier == 0) {
                motionSliceEndUs = 0; motionSliceTier = 0;
                capture::Motion m;
                // 单轮封顶,防串口/驱动异常变慢而生产者持续灌入时永久困在这里；
                // 下一轮会被仍在置位的 evtInput 立即唤醒,正常 1kHz 下通常每轮仅 1 份。
                for (int n = 0; n < 256 && capture::popMotion(m); n++) {
                    sendMouse(capture::btnState(), m.dx, m.dy, m.wheel,
                              motionGapUs(m.atUs, lastMotionStampUs));
                    g.outFrames++;
                }
            } else {
                // 固定档按原始 WM_INPUT 时间切片。Windows 把程序压到后台时,捕获线程可能
                // 10ms 才收到一批消息；旧 fetchMotion 一次吃完整批,1K 档也只剩约 100 次跳变。
                // 现在每个 sender 拍只消费一个目标时间片,把积压按原始节奏逐拍铺开。
                uint64_t intervalUs = 1000000ull / (uint64_t)tier;
                if (motionSliceTier != tier) {
                    motionSliceEndUs = 0;
                    motionSliceTier = tier;
                    lastMotionStampUs = 0;
                }
                capture::Motion first;
                if (motionSliceEndUs == 0 && capture::peekMotion(first))
                    motionSliceEndUs = first.atUs + intervalUs;
                int dx, dy, wh;
                if (motionSliceEndUs != 0)
                    capture::fetchMotionBefore(motionSliceEndUs, dx, dy, wh);
                else
                    dx = dy = wh = 0;
                if (dx || dy || wh) {
                    sendMouse(capture::btnState(), dx, dy, wh,
                              motionGapUs(motionSliceEndUs, lastMotionStampUs));
                    g.outFrames++;
                }
                capture::Motion remain;
                if (capture::peekMotion(remain)) {
                    uint64_t nextEnd = motionSliceEndUs + intervalUs;
                    // 真正的输入空档不需要按历史空白慢慢回放；只对相邻/成簇报告保留节奏。
                    uint64_t gapLimit = std::max<uint64_t>(intervalUs * 2, 2000);
                    motionSliceEndUs = remain.atUs > nextEnd + gapLimit
                                     ? remain.atUs + intervalUs : nextEnd;
                } else {
                    motionSliceEndUs = 0;
                }
            }
        }

        uint64_t now = nowUs();
        // 状态自愈:每 500ms 拿系统真值回读一遍物理键,清掉"丢了抬起沿"的残留。
        //   不修就会被下面的 100ms 刷新帧一路重发 → 板端触点永久卡住(用户反馈
        //   "不动键鼠也一直触发按键")。独占吞入时系统按键表不更新,回读不可信 → 跳过。
        if (fwd && !g.grabbed && now - lastHeal > 500000) {
            if (capture::healPhysical()) g.log(L"检测到残留按键(丢了抬起沿),已自动释放");
            lastHeal = now;
        }
        if (now - lastRefresh > 100000) {
            // 100ms 全状态刷新帧(兼作心跳):发**当前**键鼠状态而非全零。
            //   作用①喂板端断链检测;②自愈 —— UART 偶发误码(实测约 1/300,连接瞬间尤甚)
            //   吃掉一个抬起沿时,下一拍刷新帧即纠正,板端卡键/卡触点最多存活 100ms。
            //   ⚠️ 历史 bug:这里曾发 sendMouse(0,...) 全零帧,转发中按住的鼠标键每逢
            //   100ms 空闲就被强制抬起 —— 正是"卡触点/映射断续"的真因之一,绝不能回退。
            if (fwd) {
                uint8_t mod, keys[6];
                capture::keyState(mod, keys);
                sendKeys(mod, keys);
                sendMouse(capture::btnState(), 0, 0, 0);
            } else {
                sendMouse(0, 0, 0, 0);           // 非转发:状态本就是全零,纯心跳
            }
            lastRefresh = now;
        }
        if (now - lastPing > 1000000) {          // 1s PING(RTT)
            sendPing();
            lastPing = now;
        }
        // 写截断上报:这是"板端校验错误 + 乱触发按键"的**唯一 PC 侧线索**。
        //   历史上 link::write 的返回值三处全丢,截断完全静默 —— 用户只看到面板"校验错误"
        //   一直红、日志里干干净净,没法排查。现在明确写进日志并给出处置建议。
        // 只在**真的发生截断**时才报,并把当时的档位/波特率/占用率一并写进日志 —— 自带诊断,
        //   不用再回来问用户要信息。不做"事前带宽预警":8K@921600 稳态占用约 69%,本就是设计内
        //   的默认配置,按阈值提前喊会变成对所有默认用户狼来了。
        uint32_t tr = g.txTrunc.load();
        if (tr != truncSeen && now - lastTruncLog > 2000000) {
            int bd  = g.boardBaud.load();
            int hz  = tier > 0 ? tier : 1000;
            int frameBytes = g.mouseV2.load() ? 9 : 8;
            int pct = bd > 0 ? (int)((int64_t)hz * frameBytes * 10 * 100 / bd) : 0; // 8N1=10bit/B
            g.log(L"⚠ 串口发送被截断 " + std::to_wstring(tr - truncSeen) + L" 次(累计 " +
                  std::to_wstring(tr) + L")——当前 " + std::to_wstring(hz) + L" Hz @ " +
                  std::to_wstring(bd / 1000) + L" Kbps,位移帧占用约 " + std::to_wstring(pct) +
                  L"%。链路来不及发,板端可能出现校验错误/乱触发。"
                  L"处置:降低发送档位,或在设置里开启 3 Mbps 提速。");
            truncSeen = tr;
            lastTruncLog = now;
        }
    }
    timeEndPeriod(1);
}

}  // namespace

namespace sender {

void start() {
    s_stop = false;
    s_thread = std::thread(loop);
}

void releaseAll() {
    std::lock_guard<std::mutex> cycleLock(s_cycleMx);
    uint8_t zero[6] = {};
    if (link::txReady()) {
        sendKeys(0, zero);
        sendMouse(0, 0, 0, 0);
    }
    capture::resetState();
}

void stop() {
    releaseAll();
    s_stop = true;
    if (capture::evtInput) SetEvent(capture::evtInput);
    if (s_thread.joinable()) s_thread.join();
}

}  // namespace sender
