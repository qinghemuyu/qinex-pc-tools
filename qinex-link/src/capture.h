// 输入捕获:Raw Input(RIDEV_INPUTSINK 旁路累积)+ LL 钩子(独占模式吞本机输入)
//
// 架构结论:
// - 鼠标(2026-07-15 实测):LL Hook return 1 后 Raw Mouse 仍照常送达,
//   所以独占鼠标 = Hook 吞本机 + Raw Input 捕获转发。
// - 键盘(2026-07 用户实机反馈):后台/最小化时 Hook 已吞本机键盘,Raw Keyboard 却可能
//   不再到达。独占键盘必须由 LL Hook 自己投递扫描码并更新转发状态,不能套用鼠标结论。
// - 全局收放热键由 LL Hook + Raw Input 双路检测,共享按下锁去重。不能用“Hook 句柄非空”
//   当可靠性证明:低级 Hook 超时会被系统静默移除,此时 Raw Input 必须仍能接管。
#pragma once
#include <windows.h>
#include <cstdint>

namespace capture {

void start();   // 起捕获线程(消息窗 + Raw Input + LL Hook;独占键盘由 Hook 直送)
void stop();

// 边沿事件:按键/鼠标键状态变化的快照,发送线程按序 flush,保证不丢按下/抬起沿
struct Edge {
    bool    isKey;      // true = 键盘(mod+keys),false = 鼠标键(btn)
    uint8_t btn;
    uint8_t mod;
    uint8_t keys[6];
};
bool popEdge(Edge& e);
// 鼠标位移按原始报告逐份排队。直通档逐份 pop,避免发送线程短暂落后时把数个 1ms
// 报告合成一帧、位移总量虽没丢但时间颗粒退化成 250/500Hz；固定档才主动聚合。
struct Motion {
    int dx;
    int dy;
    int wheel;
    uint64_t atUs;   // 原始 WM_INPUT 入队时刻(毫秒时间扩展到 µs,同毫秒报告按 125µs 排序)
};
bool popMotion(Motion& m);                        // 直通档:逐报告取走
bool peekMotion(Motion& m);                       // 固定档:查看队首,不取走
void fetchMotionBefore(uint64_t cutoffUs, int& dx, int& dy, int& wheel); // 固定档:只聚合本发送时间片
uint8_t btnState();                                // 合流后鼠标键位图(物理∪网络,物理按屏蔽过滤)
void keyState(uint8_t& mod, uint8_t keys[6]);      // 合流后键盘状态(周期性全状态刷新帧用)
void resetState();                                 // 停转发时清全部状态(物理+网络,防板端卡键)
bool healPhysical();                               // 回读系统真值,清掉丢了抬起沿的残留物理键(仅 !grabbed 时可信)
void resetHotkeyLatch();                           // 窗口重新激活/换绑后清热键防重锁,自愈丢失的抬起沿
// 任务栏控制岛悬停时屏蔽物理鼠标转发，避免点击 UI 的同时点到手机；真正独占时自动忽略。
void setUiMouseBlocked(bool blocked);

// ── kmboxNET 模拟(netserver)对接口 ──
void injectNetMouse(uint8_t btn, int dx, int dy, int wheel, bool absButtons);  // 网络鼠标注入,合流转发
void injectNetKey(uint8_t mod, const uint8_t keys[6]);                          // 网络键盘注入(全量覆盖网络源)
void setPhysMask(uint8_t mask);                    // 屏蔽物理鼠标位图(bit0-4键 bit5X bit6Y bit7滚轮)
void fetchPhysMotion(int& dx, int& dy, int& wheel);// monitor:取走物理增量(不受屏蔽影响)
void physState(uint8_t& btn, uint8_t& mod, uint8_t keys[6]);  // monitor:当前物理键鼠状态
void setMonitorCallback(void (*cb)());             // 注册物理输入到达回调(netserver 用来唤醒推送)
void notifyMonitor();                              // 由 onRaw* 调用

extern HANDLE evtInput;   // 直通档唤醒事件

}  // namespace capture
