// overlay.h - 进程内覆盖层（真·游戏内绘制：MinHook inline hook d3d9 渲染函数）
#pragma once
#include <cstdint>
#include <windows.h>

namespace ra3 {

struct OverlayConfig {
    bool  visible   = false;   // UI 显隐（INSERT 切换）
    float scale     = 1.0f;    // 界面缩放
    float alpha     = 0.95f;   // 窗口不透明度
    int   addAmount = 10000;   // F7 每次加钱金额
    bool  blockInput = true;   // 鼠标在菜单上时，吞掉点击（不让游戏也收到）
    int   posMode    = 0;      // 落点来源：0=选中单位 1=视角中心(缓存) 2=游戏光标
    bool  diskLog    = false;  // 详细日志落盘（崩溃定位）：写 <DLL 目录>\ra3copilot_log.txt
    bool  showSelDiag = false; // 输出面板里显示「选中单位调试详情」
};

extern OverlayConfig g_cfg;

// 工作线程入口：装功能钩子 -> 建自己的窗口与设备 -> 渲染循环
void OverlayInit();

struct OverlayStatus {
    HWND  wnd        = nullptr;
    void* device     = nullptr;   // IDirect3DDevice9*
    bool  wndCreated = false;
    bool  devCreated = false;
    bool  imguiReady = false;
};
extern OverlayStatus g_ov;

// ---- 日志桥（供 actions.cpp 写进界面「输出记录」）----
enum { OVLOG_INFO = 0, OVLOG_OK, OVLOG_WARN, OVLOG_ERR };
void OverlayLogLine(int kind, const char* text);

// 取最近捕获的 d3d9 视图矩阵（相机），供"视角中心"落点用；返回是否有效
bool GetViewMatrix(float out[16]);
// 取 view+projection+viewport（供"光标处生成"反投影用）
bool GetCamData(float view[16], float proj[16], int vp[4]);
int  ViewRejects();   // 被"3D相机过滤"拒掉的视图矩阵次数
// vtable 诊断：out[0..4]=vt16/17/42/44/45 函数地址，[5]=device，[6]=d3d9.dll 基址，[7]=VIEW 是否来自 hook
void GetVtInfo(uint32_t out[8]);
// 原始 VIEW 矩阵诊断（不过滤）：lastMat/minMat 各 16 float；末次/最小的 _33；调用次数
void GetRawView(float lastMat[16], float* lastFz, float minMat[16], float* minFz, int* count);

// 调试接口用：
int  OverlayLogDump(char* buf, int n, int maxLines);  // 导出输出记录尾部
void OverlaySetCrate(int idx, int count);              // 设置空投箱类型/数量
const char* DiskLogPath();                             // 日志落盘文件完整路径（设置页显示用）
const char* DllDir();                                  // 本 DLL 所在目录（带结尾反斜杠）

// ---- 单位召唤 / 空投箱（后端；当前为占位实现，见 overlay.cpp 末尾）----
// hash = 支援单位对象类型哈希（见 units_corona.h）；ownerPlayer 0=己方,1..8=玩家N
bool SpawnSupportUnit(uint32_t hash, int count, int ownerPlayer, bool atMouse, const char* name);
bool DropCrate(int ownerPlayer, int count, bool atMouse);

} // namespace ra3
