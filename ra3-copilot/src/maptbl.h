#pragma once

// 运行时解析当前关卡的 .map 脚本，生成"目标 -> 完成条件"表。
//
// 产出与 src/generated/objectives_data.h 完全同构的 token 行（M|/O|），
// 所以消费端（ObjTableInit）不需要区分来源。
//
// 为什么要走这条路：内置表只覆盖导出过的那 27 张官方战役图，mod 图、其他版本、
// 玩家自制图都没有。而 .map 就在 .big 里，格式可解析 —— 不依赖内存结构，
// 也不依赖任何硬编码偏移。

struct MapTblResult {
    const char** lines;     // 以 nullptr 结尾
    int lineCount;
    int mapCount;
    int objCount;
    int pathCount;
    int scriptCount;        // 扫过的脚本数，用于判断解析是否真的走通了
    char mapName[96];
    char source[300];       // 实际读取的 big 条目 / 散文件路径
    char err[200];
};

// relHint = GameLogic+0x98 给的相对路径（可 nullptr）；mapBaseName = 不含扩展名的地图名。
// 失败时返回 false，err 里写原因，lines 为空。
bool MapTblBuild(const char* relHint, const char* mapBaseName, MapTblResult* out);
void MapTblFree(MapTblResult* r);
