// httpapi.h - 进程内调试接口（只监听 127.0.0.1，供开发期实时验证 + MCP「脑机接口」）
//
// 设计要点：
//   · HTTP 服务跑在**独立线程**；只读查询（state/diag/log/snapshot/units）在该线程直接答
//     （读内存跨线程安全；战场枚举也只读内存，不调游戏函数）
//   · **会改游戏的操作**（召唤/复制/收编/升级/写内存/调游戏函数）不能跨线程做
//     → 投递到命令槽（HttpApiPump 在渲染线程=游戏主线程里执行），HTTP 线程等结果（带超时）
//   · **无闸门**（2026-10-16 改）：接口仅监听 127.0.0.1，单机环境下「把 MCP 配置交给 AI」
//     本身即视为授权，因此不再拦截变更请求。`ai=1` 仅作遥测（计数 + 日志）供面板显示。
#pragma once

namespace ra3 {

// 启动/停止（默认端口 17832，仅 127.0.0.1）
void HttpApiStart(int port = 17832);
void HttpApiStop();

// ★ 由渲染线程每帧调用：执行挂起的命令（没有就立刻返回）
void HttpApiPump();

// ---- 脑机接口（MCP）开放开关 & 遥测（面板用）----
bool AiOpen();
void SetAiOpen(bool on);
int  AiCallCount();      // 已放行的 AI 调用次数
int  AiRejectCount();    // 因未开放被拒的次数
const char* HttpListenLine();   // "127.0.0.1:17832" 或 "未监听"
// 最近 AI 调用记录（多行文本），返回写入长度
int  AiLogDump(char* buf, int n, int maxLines);
// 记一条 AI 调用（供其它模块记录非 HTTP 来源的动作）
void AiLogLine(const char* fmt, ...);

} // namespace ra3
