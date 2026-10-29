// 发送线程:回报率档位节拍(时间窗聚合)/ 直通(逐事件),边沿帧立发,100ms 心跳,1s PING
#pragma once

namespace sender {
void start();
void releaseAll();   // 同步发送键盘/鼠标全抬起并清本机状态;断开/退出前调用
void stop();
}
