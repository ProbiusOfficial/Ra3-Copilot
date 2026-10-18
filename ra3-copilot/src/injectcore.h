// injectcore.h - 注入 / 卸载 / 状态探测的共用实现（x86）
//
// 供 injector.exe（命令行）与 Ra3Copilot.exe（图形启动器）共用：
//   - 按进程名找游戏
//   - 模块表 / HTTP 双重判活
//   - CreateRemoteThread(LoadLibraryW) 注入
//   - POST /unload + 等模块消失（真卸载）
#pragma once
#include <windows.h>
#include <string>

namespace inj {

extern const wchar_t* const kGameProc;
extern const wchar_t* const kDllName;
constexpr int kHttpPort = 17832;

DWORD FindGamePid();
bool  IsElevated();

// 1=已加载 0=未加载 -1=查询失败（权限 / 进程已退出）
int   QueryModule(DWORD pid, const wchar_t* name);
bool  WaitModule(DWORD pid, const wchar_t* name, bool want, int timeoutMs);

// 最小 HTTP 客户端（127.0.0.1:17832）：raw request / GET path
bool  HttpTalk(const char* request, char* resp, int rn);
bool  HttpGet(const char* path, char* resp, int rn);

// 注入并复核（模块表出现 + /ping 应答）。message 为面向用户的中文结果。
bool  Inject(DWORD pid, const wchar_t* dllPath, std::wstring& message);
// 请求游戏内实例卸载，并等待模块从进程消失。
bool  Unload(DWORD pid, const std::wstring& dllName, std::wstring& message);

std::wstring ProcessPath(DWORD pid);

} // namespace inj
