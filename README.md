# Ra3-Copilot

红色警戒 3 日冕 Mod（`ra3_1.12.game`，v1.12.3444.25830，32 位）的**注入型进程内作战辅助系统**——以局内覆盖层（overlay）形式接入游戏，按 `INSERT` 呼出菜单，提供资源 / 战斗 / 单位 / 召唤 / 空投等辅助能力，并通过 **MCP 脑机接口**把战局开放给 AI。

> 仅用于单机学习与研究。**版本强绑定**：hook RVA 硬编码于 v1.12.3444.25830，注入前执行 AOB 字节级校验（fail-closed，不符绝不 patch）。
> 适配基线：日冕 Mod v1.12；其他版本需重新定位 RVA / AOB。

**官网** → <https://probiusofficial.github.io/Ra3-Copilot/>

## 功能一览

| 分类 | 能力 |
|---|---|
| 渲染 | MinHook inline-hook d3d9 `Present/EndScene`，ImGui 直接画在游戏画面内（不抢焦点、切窗口不静默） |
| 资源 | 加钱 / 设置资金、电力满、科技点满、全科技、快速建造、敌方无法建造 |
| 战斗 | 玩家无敌、无限弹药（赋予后取消选中仍保持）、超武就绪 / 禁用敌方超武、危险等级干预、全图去迷雾、秒杀模式、无限缩放 |
| 单位 | 选中单位变速 / 改血 / 收编 / 摧毁 / 满级；单位内部结构可整段复制留档 |
| 协议 | 秘密协议技能与兵种升级直授（294 项 `SpecialPower_` / 81 项 `PlayerTech_`），按阵营列目录、门槛明示 |
| 召唤 | 按单位代码（对象类型哈希）投放单位与建筑；海面折跃信标按兵种（海军 / 空军 / 两栖）抽取种子 |
| 空投 | 现金 / 治疗 / 晋升箱、海陆投仓，支持周期自动空投 |
| 感知 | 全图活体对象枚举（单位 / 建筑 / AI / 坐标 / 血量，按归属分组，零人工点选）；`/objects` `/obj` `/snapshot` 即席查询与页级差分 |
| 指挥 | AIUpdate 直调：移动 / 攻击 / 建造（`/aicall`）；`/place` 按几何打分给建筑落点候选，引擎只做最终校验 |
| 接口 | MCP stdio 服务器（`ra3-copilot/mcp/`，19 个工具）：AI 看局势、查单位、召唤、空投、动单位、改全局；注入即常驻监听，只绑 `127.0.0.1` |
| 辅助 | 光标落点（真实世界坐标）、本地玩家追踪、实时资金显示、带时间戳的操作日志区 |
| 调试 | 进程内 HTTP 接口 `127.0.0.1:17832`（55 条路由：`/state` `/read` `/peek` `/summon` `/poke` `/call` `/aicall` `/protocol` `/grant` `/upgrade` `/place` `/snapshot` …） |

## 快速开始

要求：**VS2022 BuildTools（x86 工具链 + MASM `ml.exe`）+ Windows SDK 10.0.22621**

```bat
ra3-copilot\build.bat
```

产物（均为 x86——游戏是 32 位进程，注入器必须同位数）：

- `ra3-copilot\build_out\Ra3Copilot.exe` —— **单文件发布版**（启动器 + DLL + MCP 自解压）
- `dist\Ra3Copilot.exe` —— 对外发布件（与上面同文件，单独放一份便于分发）
- `ra3copilot.dll` —— 注入模块（开发 / 热重载用）
- `injector.exe` —— 命令行注入器（开发用）

> `build.bat` 不调用 `vcvars32.bat`（其内部调用的 `reg.exe` 会被部分环境的安全策略拦截），而显式设置 `PATH/INCLUDE/LIB`。
> `.bat` 必须保持 CRLF 行尾 + 纯 ASCII 内容，仓库已通过 `.gitattributes` 保证检出为 CRLF。

改 hook / 改 AOB 后，交付前先用真实游戏 exe 做字节级预检：

```bat
python ra3-copilot\tools\precheck_aob.py --game "<ra3_1.12.game 路径>"
rem 也可用环境变量 RA3_GAME_EXE 指定路径
```

## 发版

打 tag 即由 GitHub Actions（`windows-latest`，vcvars x86 工具链）构建并发布：

```bash
git tag v1.0.0
git push origin v1.0.0
```

产物：`Ra3Copilot-<tag>-win-x86.zip`（启动器 + DLL + 注入器 + 说明）、单文件 `Ra3Copilot.exe`、`SHA256SUMS.txt`。

- 版本号取自 `ra3-copilot/res/launcher.rc` 的 `FILEVERSION`；与 tag 不符时 CI 只告警，exe 属性里的版本会对不上
- `workflow_dispatch` 手动触发只构建、不发布
- CI **不跑 AOB 字节级校验**（游戏 exe 不入库），兼容性以本地 `tools/precheck_aob.py` 的 25/25 为准
- CI 工具链跟随 runner（当前 MSVC 14.51），本机 `build.bat` 固定在 14.44 ⇒ **两侧产出的二进制不是同一个**（节数、体积都不同）。代码同源，但 CI 产物不等于本地实测过的那一份，对外前建议本地抽验一次

## 使用

1. 启动游戏并进入对局（遭遇战 / 战役均可）
2. 双击 `Ra3Copilot.exe`：自动识别游戏进程；点「注入修改器」即可
   - 游戏以管理员权限运行时，启动器会请求一次 UAC 提权
   - 已注入时显示「卸载修改器」，另有「重载修改器」用于更新 DLL 后重新注入
3. 游戏内 `INSERT` 显示 / 隐藏菜单，`Ctrl+F1` 加钱（默认热键，可在「设置」里改键）
4. AI 接入无需开关：接口注入即**常驻监听** `127.0.0.1:17832`；启动器「MCP 配置」可复制客户端配置

> 单文件版运行时会把 `ra3copilot.dll`、MCP 脚本与目录释放到 `%LOCALAPPDATA%\Ra3Copilot\`；
> 日志、按键配置、面板布局也都在这个目录，启动器「打开目录」可直达。

## 目录

```
├── ra3-copilot/            产品本体
│   ├── src/               sdk.h（RVA/AOB 常量表）/ hook / features / overlay / actions
│   │                      aicmd（AIUpdate 直调）/ battlefield（战场枚举）/ httpapi / symres（符号解析）
│   │                      protocol（秘密协议授予）/ stubs.asm（MASM stub）
│   │                      units_corona.h（可召唤单位代码表）/ protocols_corona.h（技能名与门槛表）
│   ├── imgui/             Dear ImGui v1.91.4（MIT）
│   ├── third_party/       MinHook 1.3.3（BSD-2-Clause）
│   ├── tools/             AOB 预检 / 注入诊断 / 渲染探针等脚本
│   ├── mcp/               脑机接口（MCP stdio 服务器 + 单位目录）
│   ├── build.bat / deploy.bat / refresh_dll.bat
│   ├── build_out/         构建产物（Ra3Copilot.exe / ra3copilot.dll）
│   └── README.md          架构 / hook 清单 / 渲染原理
├── unit-codes/            单位代码（SAGE 对象类型哈希）
│   ├── README.md / 单位代码与哈希算法.md   结论与完整报告
│   ├── data/              派生清单（.tsv / .h）
│   └── tools/             提取逻辑：哈希算法 / 图鉴拉取 / 清单生成 / 建站
└── docs/                  局内渲染 / 单位操作与召唤 / 召唤状态字根因 / HTTP 调试接口
                           秘密协议授予 / AI 建筑摆放 / 无限缩放 / MCP 脑机接口
                           AIUpdate 直调指挥 / 开发铁律 / 美术资源清单
```

官网源码不在这棵树上 —— 它单独占 `gh-pages` 分支（`index.html` / `units.html` / `assets/`），
改动提交后由该分支直接发布。

## 单位代码

游戏里每个单位 / 建筑 / 对象都有一个 32 位**对象类型哈希**，创建对象时用它查模板：

```
代码 = StringHasher.Hash( 对象模板名.lower() )     // SAGE 引擎的 SuperFastHash 变体
```

本项目把日冕全量 **5461 条**对象代码整理成表并可离线复算，其中「可召唤且能用」**411 条**。

- 在线查询 → <https://probiusofficial.github.io/Ra3-Copilot/units.html>
- 离线查询 → `gh-pages` 分支的 `units.html`（纯静态，支持按名字离线算代码）

## 第三方组件

- [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.3 —— BSD-2-Clause（`ra3-copilot/third_party/`）
- [Dear ImGui](https://github.com/ocornut/imgui) v1.91.4 —— MIT（`ra3-copilot/imgui/`）

## 仓库边界

`main` 分支收录 **产品本体（`ra3-copilot/`）、单位代码研究（`unit-codes/`）、文档（`docs/`）与发版工作流（`.github/`）**；
官网源码在 `gh-pages` 分支，站点地址见上。

游戏原始数据抽取物与第三方图鉴数据（`unit-codes/data/source/`）不随仓库分发，可由 `unit-codes/tools/` 自行重新生成。
