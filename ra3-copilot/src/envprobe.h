// envprobe.h - 环境/版本采集（GET /env）
#pragma once

namespace ra3 {

// 把「能区分这台机器」的信息一次性采集出来：游戏 exe 的 PE 头（TimeDateStamp 能区分补丁）、
// d3d9 的真实来源与版本、本地代理 / DXVK / ReShade / 叠加层、驱动模块、显示模式。
// 只读，可在 HTTP 线程直接调。
void EnvProbeText(char* out, int n);

} // namespace ra3
