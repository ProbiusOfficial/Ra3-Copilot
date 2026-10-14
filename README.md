# Ra3-Copilot

红色警戒 3 日冕 Mod（`ra3_1.12.game`，v1.12.3444.25830，32 位）的**注入型进程内修改器**——以局内覆盖层（overlay）形式接入游戏，按 `INSERT` 呼出菜单，提供资源 / 战斗 / 单位 / 召唤 / 空投等辅助能力。

> 仅用于单机学习与研究。**版本强绑定**：钩子 RVA 硬编码于 v1.12.3444.25830，注入前执行 AOB 字节级校验（fail-closed，不符绝不 patch）。

## 功能一览

| 分类 | 能力 |
|---|---|
| 渲染 | MinHook inline-hook d3d9 `Present/EndScene`，ImGui 直接画在游戏画面内（不抢焦点、切窗口不静默） |
| 资源 | 加钱 / 设置资金、电力满、科技点满、全科技、快速建造、敌方无法建造 |
| 战斗 | 玩家无敌、选中单位弹药满、超武就绪 / 禁用敌方超武、危险等级干预、全图去迷雾 |
| 单位 | 选中单位变速 / 改血 / 收编 / 摧毁；召唤单位与基地车；空投箱子（支持按周期自动空投） |
| 辅助 | 光标落点（真实世界坐标）、本地玩家追踪、实时资金显示、带时间戳的操作日志区 |
| 调试 | 进程内 HTTP 接口 `127.0.0.1:17832`（`/state` `/read` `/peek` `/summon` `/poke` `/call` …） |

## 构建

要求：**VS2019 BuildTools（x86 工具链 + MASM `ml.exe`）+ Windows SDK 10.0.19041**

```bat
trainer-v2\build.bat
```

产物（均为 x86——游戏是 32 位进程，注入器必须同位数）：

- `ra3trainer.dll` —— 注入模块
- `injector.exe` —— 注入器

> `build.bat` 不调用 `vcvars32.bat`（其内部调用的 `reg.exe` 会被本机安全策略拦截），改为显式设置 `PATH/INCLUDE/LIB`。
> `.bat` 必须保持 CRLF 行尾 + 纯 ASCII 内容，仓库已通过 `.gitattributes` 保证检出为 CRLF。

## 使用

1. 启动游戏并进入对局（遭遇战 / 战役均可）
2. 运行 `trainer-v2\deploy.bat`（自动 UAC 提权 → 注入 → 打印诊断）
3. 游戏内 `INSERT` 显示 / 隐藏菜单，`F7` 加钱

## 目录

```
├── trainer-v2/            产品本体
│   ├── src/               sdk.h（RVA/AOB 常量表）/ hook / features / overlay / actions / stubs.asm（MASM 壳）
│   ├── imgui/             Dear ImGui v1.91.4（MIT）
│   ├── third_party/       MinHook 1.3.3（BSD-2-Clause）
│   ├── tools/             AOB 预检 / 注入诊断 / 渲染探针等脚本
│   ├── build.bat / deploy.bat / refresh_dll.bat
│   └── README.md          架构 / 钩子清单 / 渲染原理（详细版）
└── docs/                  局内渲染方案 / 单位操作与召唤 / 召唤状态字根因 / HTTP 调试接口 / LLM 接入调研
```

## 第三方组件

- [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.3 —— BSD-2-Clause（`trainer-v2/third_party/`）
- [Dear ImGui](https://github.com/ocornut/imgui) v1.91.4 —— MIT（`trainer-v2/imgui/`）

## 仓库边界

