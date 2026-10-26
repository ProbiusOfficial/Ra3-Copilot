#pragma once
#include <stdint.h>

// ---- BIG4 读取（RA3/CNC3 明文变体）----
//
// 头：char "BIG4"; u32 totalSize(LE); u32 count(BE); u32 indexSize(BE)
// 索引：count × { u32 offset(BE); u32 size(BE); char name[] NUL 结尾 }
//
// 只做"按文件名找一个条目并读出原始字节"，不做解压 —— 条目自身可能是 refpack，
// 由调用方再解一层。索引读一次就缓存，换关不会重复扫盘。

struct Big4Entry {
    char     bigPath[260];   // 空 = 条目是磁盘上的散文件（mod 覆盖）
    uint32_t offset;         // 散文件时为 0
    uint32_t size;           // 散文件时为 0，由读取时取实际大小
    char     name[200];      // 完整相对路径（小写，'\\' 分隔）
    bool     loose;
};

// 按文件名找 .map。先看散文件（游戏优先用磁盘上的同名文件，mod 常这么覆盖），
// 再扫 exe 目录及其 Data 子目录下的 .big 索引。
// relHint 是 GameLogic+0x98 给的相对路径，仅用于在多个同名条目里挑后缀最匹配的；
// 可传 nullptr。成功返回 true。结果按 (文件名, relHint) 缓存。
bool Big4FindMap(const char* relHint, const char* mapBaseName, Big4Entry* out);

// 读条目原始字节。返回调用方需用 Big4BufFree 释放的缓冲，失败返回 nullptr。
void* Big4ReadEntry(const Big4Entry& e, uint32_t* outSize);
void  Big4BufFree(void* p);

// 指定根目录（默认取宿主进程 exe 所在目录）。launcher / 测试程序需要显式设置。
void  Big4SetRootDir(const char* dir);

void  Big4ResetCache();
