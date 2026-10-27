// buildinfo.h - 目标二进制构建的识别、决议与双列取值
//
// 零售版与 Steam TUC 版共用同一个版本号（1.12.3444.25830），版本号本身区分不出来。
// 两者是不同编译器产出的两份二进制，代码布局整体错位（30 处 hook 站点差
// 0x3BD60~0x3F060，不是整体平移），所以地址必须按构建分两列存。
// 判据取 PE 头的 SizeOfImage：零售 0x93B000 / Steam 0x950000。
//
// 取错一列的后果不是"功能失效"而是 patch 到无关指令上 —— 决议必须发生在任何 hook
// 安装之前；未知构建按零售取，由 AOB 逐条校验兜底（不符即跳过，绝不写错字节）。
#pragma once
#include <windows.h>
#include <cstdint>

namespace ra3 {

enum class BuildKind : int {
    Unknown = 0,
    Retail  = 1,   // 零售 1.12（SizeOfImage 0x93B000，9306120 字节）
    Steam   = 2,   // Steam TUC 1.12（SizeOfImage 0x950000）
};

struct BuildProbe {
    uint32_t sizeOfImage = 0;
    uint32_t imageBase   = 0;
    uint64_t fileSize    = 0;
    uint32_t timeStamp   = 0;   // 链接时间，仅用于诊断输出
};

// 纯判定，不做 IO。无法识别返回 Unknown。
BuildKind ClassifyBuild(const BuildProbe& p);

// PE 头探测：同进程镜像 / 跨进程镜像（启动器在游戏外）/ 磁盘文件（游戏未启动时）
bool ProbeImage(uintptr_t imageBase, BuildProbe* out);
bool ProbeImageProcess(HANDLE proc, uintptr_t imageBase, BuildProbe* out);
bool ProbeFile(const wchar_t* path, BuildProbe* out);

// 全局决议：进程内唯一。DLL 在 OverlayInit 开头调一次，之后所有取值都读它。
// forced=true 表示玩家在启动器里指定，而不是自动判定。
void        SetBuild(BuildKind kind, bool forced);
BuildKind   GetBuild();
bool        BuildOverridden();
const char* BuildName(BuildKind k);          // "零售 1.12" / "Steam 1.12" / "未知"

// DLL 侧一步到位：探测**当前进程主模块**（只对注入进游戏进程的 DLL 成立）+ 读 ini 强制值，
// 决议后写入全局。必须在装任何 hook 之前调用。
BuildKind ResolveBuildForSelf(const wchar_t* iniPath, BuildProbe* probeOut);

// 双列取值：按当前构建选 retail / steam。
inline uint32_t Pick(uint32_t retail, uint32_t steam) {
    return GetBuild() == BuildKind::Steam ? steam : retail;
}

// ra3copilot_build.ini（裸文本单行键值，与 ra3copilot_keys.ini 同风格）：
//   mode=auto|retail|steam
// 返回 true 表示写明了强制构建（force 被填）；false 表示 auto 或文件不存在。
bool LoadBuildIni(const wchar_t* iniPath, BuildKind* force);
void SaveBuildIni(const wchar_t* iniPath, const char* mode);   // "auto"/"retail"/"steam"

} // namespace ra3
