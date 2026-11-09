// campaign_patch.h - 战役扩展与全科技模式：数据包等长原地补丁
//
// 设计要点（改动前务必读）：
//  * 公开库不带任何第三方数据。exe 里只内嵌我们自己的 Lua 片段（IDR_CAMPAIGN_LUA），
//    密钥、索引、目标条目全部在运行时从玩家本机的数据包里读出来。
//  * 只改 data\scripts\scriptevents.xml 这一条目的内容，长度严格不变。
//    容器索引、其它条目、文件总长一律不动 —— 重建整包会让游戏启动即退出。
//  * 加密是 C[i] = (P[i] + K[i % 512]) & 0xFF，K 与绝对文件偏移绑定。
//    读/写都从 512 对齐的 base 起算，保证相位一致。
//  * 启用 = 注释块等长压缩 + 插入 Lua；还原 = 优先按备份字节写回，
//    否则把精确 snippet 区间替换成等长空白（Lua 语义等价，无需外部备份）。
#pragma once

#include <string>
#include <cstdint>

namespace cup {

enum State {
    kNotFound = 0,   // 没找到可用的数据包
    kOriginal = 1,   // 原版（未启用）
    kEnabled  = 2,   // 已启用
    kMismatch = 3,   // 内容/版本不匹配，拒绝写入
    kError    = 4,   // 读写失败
    kOutdated = 5,   // 打过旧版片段，需要重应用升级（仍视为已启用）
};

struct Info {
    int state = kNotFound;
    std::wstring path;      // 命中的数据包路径
    std::wstring detail;    // 给 UI 看的一行说明
    uint64_t base = 0;
    uint32_t size = 0;
    bool outdated = false;  // 打了我们旧版片段：勾选不变，但启动游戏前要重应用一次
};

// 只读探测：按体积降序试数据目录下的 *.lyi，找到含目标条目的那个。
// explicitPack 非空时优先直接试这个文件（用户可手工指定）。
// 结果会缓存在模块内，SetEnabled 直接用它，避免重复扫描。
Info Probe(const std::wstring& gameDir, const std::wstring& runtimeDir,
           const std::wstring& explicitPack = L"");

// 幂等地切到启用 / 还原状态。写前会在运行目录留一份原始区域备份。
// 成功时 msg 是给日志的一行说明；失败时 msg 是可直接展示的原因。
bool SetEnabled(bool enable, std::wstring* msg);

// 当前缓存的状态（不重新读盘），供 UI 每帧显示。
const Info& Cached();

// 最近一次 Probe 的逐步诊断（排障用，UI 不显示）。
const std::wstring& Diag();

} // namespace cup
