// 离线验证 .map 解析：把当前构建产出的目标表 token 行打到 stdout。
// 与 src/generated/objectives_data.h 的同段逐行比对，一致才说明移植没走样。
//
//   ra3maptest.exe [游戏目录] [地图名] [地图名...]
//   ra3maptest.exe "C:\GAME\Red Alert 3" camp_a01_brightonbeach_smith camp_a02_cannes_smith
//
// 给多个地图名时在**同一个进程里**依次构建 —— 用来回归"换图后行缓冲没清干净"这类
// 只在同进程连续构建时才暴露的问题（单图跑一遍是看不出来的）。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "maptbl.h"
#include "big4.h"

#include <stdio.h>

static int BuildOne(const char* base) {
    const DWORD t0 = GetTickCount();
    MapTblResult r;
    const bool ok = MapTblBuild(nullptr, base, &r);
    const DWORD dt = GetTickCount() - t0;

    if (!ok) {
        printf("[x] %s (%ums)\n", r.err, (unsigned)dt);
        return 1;
    }

    printf("[ok] map=%s scripts=%d objs=%d paths=%d lines=%d time=%ums\n",
           r.mapName, r.scriptCount, r.objCount, r.pathCount, r.lineCount, (unsigned)dt);
    printf("[source] %s\n", r.source);

    int printed = 0;
    for (int i = 0; r.lines && r.lines[i]; ++i) {
        printf("%s\n", r.lines[i]);
        ++printed;
    }
    // 行数自检：遍历到 nullptr 的条数必须等于构建报告的 lineCount。
    // 不等就说明 g_lines 尾部还留着上一次构建的旧指针（会串关卡）。
    if (printed != r.lineCount) {
        printf("[!] 行数不一致: 遍历 %d 条 / 报告 %d 条 —— 行缓冲尾部有残留\n",
               printed, r.lineCount);
        return 2;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2) Big4SetRootDir(argv[1]);

    int rc = 0;
    if (argc < 3) {
        rc = BuildOne("camp_a01_brightonbeach_smith");
    } else {
        for (int i = 2; i < argc; ++i) {
            const int r1 = BuildOne(argv[i]);
            if (r1 && !rc) rc = r1;
        }
    }
    return rc;
}
