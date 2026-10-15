# Ra3-Copilot

红色警戒 3 日冕 Mod（`ra3_1.12.game`，v1.12.3444.25830，32 位）的**注入型进程内作战辅助系统**——以局内覆盖层（overlay）形式接入游戏，按 `INSERT` 呼出菜单，提供资源 / 战斗 / 单位 / 召唤 / 空投等辅助能力，并通过 **MCP 脑机接口**把战局开放给 AI。

> 仅用于单机学习与研究。**版本强绑定**：钩子 RVA 硬编码于 v1.12.3444.25830，注入前执行 AOB 字节级校验（fail-closed，不符绝不 patch）。
> 适配基线：日冕 Mod v1.12；其他版本需重新定位 RVA / AOB。

**官网** → <https://probiusofficial.github.io/Ra3-Copilot/>

## 功能一览

| 分类 | 能力 |
|---|---|
| 渲染 | MinHook inline-hook d3d9 `Present/EndScene`，ImGui 直接画在游戏画面内（不抢焦点、切窗口不静默） |
| 资源 | 加钱 / 设置资金、电力满、科技点满、全科技、快速建造、敌方无法建造 |
| 战斗 | 玩家无敌、选中单位弹药满、超武就绪 / 禁用敌方超武、危险等级干预、全图去迷雾 |
| 单位 | 选中单位变速 / 改血 / 收编 / 摧毁 / 满级；召唤单位与基地车；空投箱子（支持按周期自动空投） |
| 感知 | 全图活体对象枚举（单位 / 建筑 / AI / 坐标 / 血量，按归属分组，零人工点选）；`/objects` `/obj` 即席查询 |
| 指挥 | AIUpdate 直调：移动 / 攻击 / 建造（`/aicall`，均已实机验证） |
| 接口 | MCP stdio 服务器（`trainer-v2/mcp/`）：AI 看局势、查单位、召唤、空投、动单位、改全局；面板总闸默认关闭，打开才允许变更 |
| 辅助 | 光标落点（真实世界坐标）、本地玩家追踪、实时资金显示、带时间戳的操作日志区 |
| 调试 | 进程内 HTTP 接口 `127.0.0.1:17832`（`/state` `/read` `/peek` `/summon` `/poke` `/call` `/aicall` …） |

## 快速开始

要求：**VS2019 BuildTools（x86 工具链 + MASM `ml.exe`）+ Windows SDK 10.0.19041**

```bat
trainer-v2\build.bat
```

产物（均为 x86——游戏是 32 位进程，注入器必须同位数）：

- `ra3trainer.dll` —— 注入模块
- `injector.exe` —— 注入器

> `build.bat` 不调用 `vcvars32.bat`（其内部调用的 `reg.exe` 会被部分环境的安全策略拦截），改为显式设置 `PATH/INCLUDE/LIB`。
> `.bat` 必须保持 CRLF 行尾 + 纯 ASCII 内容，仓库已通过 `.gitattributes` 保证检出为 CRLF。

改钩子 / 改 AOB 后，交付前先用真实游戏 exe 做字节级预检：

```bat
python trainer-v2\tools\precheck_aob.py --game "<ra3_1.12.game 路径>"
rem 也可用环境变量 RA3_GAME_EXE 指定路径
```

## 使用

1. 启动游戏并进入对局（遭遇战 / 战役均可）
2. 运行 `trainer-v2\deploy.bat`（自动 UAC 提权 → 注入 → 打印诊断；Python 仅用于只读诊断，可用 `RA3_PY` 指定解释器）
3. 游戏内 `INSERT` 显示 / 隐藏菜单，`F7` 加钱
4. 需要 AI 接入时，在面板打开「脑机接口」总闸（详见 `docs/MCP开放-脑机接口.md`）

## 目录

```
├── site/                  项目官网（已部署 GitHub Pages）
│   ├── index.html         首页
│   ├── units.html         单位代码查询器（全量检索 + 离线复算）
│   └── assets/            全套美术资源
├── trainer-v2/            产品本体
│   ├── src/               sdk.h（RVA/AOB 常量表）/ hook / features / overlay / actions
│   │                      aicmd（AIUpdate 直调）/ battlefield（战场枚举）/ httpapi / symres（符号解析）
│   │                      stubs.asm（MASM 壳）/ units_corona.h（可召唤单位代码表）
│   ├── imgui/             Dear ImGui v1.91.4（MIT）
│   ├── third_party/       MinHook 1.3.3（BSD-2-Clause）
│   ├── tools/             AOB 预检 / 注入诊断 / 渲染探针等脚本
│   ├── mcp/               脑机接口（MCP stdio 服务器 + 单位目录）
│   ├── build.bat / deploy.bat / refresh_dll.bat
│   └── README.md          架构 / 钩子清单 / 渲染原理（详细版）
├── unit-codes/            单位代码（SAGE 对象类型哈希）
│   ├── README.md / 单位代码与哈希算法.md   结论与完整报告
│   ├── data/              派生清单（.tsv / .h）
│   └── tools/             提取逻辑：哈希算法 / 图鉴拉取 / 清单生成 / 建站
└── docs/                  局内渲染 / 单位操作与召唤 / 召唤状态字根因 / HTTP 调试接口
                           MCP 脑机接口 / AIUpdate 直调指挥方案 / 美术资源清单
```

## 单位代码

游戏里每个单位 / 建筑 / 对象都有一个 32 位**对象类型哈希**，创建对象时用它查模板：

```
代码 = StringHasher.Hash( 对象模板名.lower() )     // SAGE 引擎的 SuperFastHash 变体
```

本项目把日冕全量 **5461 条**对象代码整理成表并可离线复算，其中「可召唤且能用」**410 条**。

- 在线查询 → <https://probiusofficial.github.io/Ra3-Copilot/units.html>
- 本地查询 → `site/units.html`（纯静态、离线可用，支持按名字离线算代码）

## 第三方组件

- [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.3 —— BSD-2-Clause（`trainer-v2/third_party/`）
- [Dear ImGui](https://github.com/ocornut/imgui) v1.91.4 —— MIT（`trainer-v2/imgui/`）

## 仓库边界

本仓库收录 **Ra3-Copilot 官网（`site/`，已部署 GitHub Pages）、产品本体（`trainer-v2/`）、单位代码研究（`unit-codes/`）与文档（`docs/`）**。

游戏原始数据抽取物与第三方图鉴数据（`unit-codes/data/source/`）亦不随仓库分发，可由 `unit-codes/tools/` 自行重新生成。
