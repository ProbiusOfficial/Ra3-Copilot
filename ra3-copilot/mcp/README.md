# Ra3-Copilot MCP 服务器（脑机接口）

把红警3 日冕修改器的进程内接口包装成 **标准 MCP 工具**，让任意支持 MCP 的 AI 客户端
读取你的战局、并给你投放单位/箱子。

```
ra3_mcp.py            零依赖 MCP stdio 服务器（只用 Python 标准库）
unit_catalog.json     1008 单位 + 54 箱子（由 tools/gen_mcp_catalog.py 生成）
```

## 客户端配置

```json
{
  "mcpServers": {
    "ra3-copilot": {
      "command": "python",
      "args": ["<本目录绝对路径>/ra3_mcp.py"]
    }
  }
}
```

游戏内 `INSERT` → 「脑机接口」面板里有一键复制的配置片段（路径自动填好）。

## 前置条件

1. 游戏在跑，且修改器 DLL 已注入（`deploy.bat`）；
2. 修改器接口随 DLL 注入即**常驻监听** `127.0.0.1:17832`（无开关；安全边界靠只绑本机回环）。

## 工具

| 工具 | 说明 |
|---|---|
| `get_battle_state` | 我方/敌方部队建筑数、敌方阵营与构成、资金、选中单位 |
| `get_enemy_report` | 只返回敌方明细 |
| `search_units` | 单位检索（中文名 / 拼音首字母 / 英文模板名 / 哈希） |
| `summon_unit` | 召唤单位（名字/模板名/哈希，count ≤ 30）；落点用 `place=view\|selection\|cursor` |
| `get_spawn_point` | 查可用落点：`view` 视野中心（玩家在看的位置）/ `selection` 选中单位 / `cursor` 鼠标 / `auto` |
| `drop_crate` | 空投箱子（治疗 / 现金 / 晋升 / 属性箱…） |
| `selected_action` | 复制 / 增援 / 基地车 / 收编 / 摧毁 / 满级 |
| `set_units` | 调速（fast/slow/freeze/restore）/ 改血（god/hurt/restore） |
| `toggle_feature` | 电力无限 / 全科技 / 免费建造 / 去迷雾 / 无敌 / 秒杀… |
| `set_freebuild` | 免费建造 |
| `get_log` / `get_status` | 事件日志 / 接口状态 |
| `raw` 🎛️ | 任意内存读写（需 `RA3_MCP_ALLOW_RAW=1` 才注册） |

## 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `RA3_API` | `http://127.0.0.1:17832` | 修改器接口地址 |
| `RA3_MCP_ALLOW_RAW` | `0` | 置 1 才注册危险的 `raw` 工具 |

## 自检

```bash
python ra3_mcp.py --selftest
```

（协议自检：跑一遍 initialize / tools/list，并检索"磁爆坦克"。）

详见 `../../docs/MCP开放-脑机接口.md`。
