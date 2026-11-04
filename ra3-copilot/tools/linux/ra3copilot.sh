#!/usr/bin/env bash
# 在「游戏所在的 Proton prefix」里运行 Ra3-Copilot 的命令行注入器。
#
#   ./ra3copilot.sh            # 注入
#   ./ra3copilot.sh unload     # 卸载
#   ./ra3copilot.sh status     # 只看游戏进程
#   ./ra3copilot.sh ping       # 查接口
#   ./ra3copilot.sh launch %command%   # Steam 启动项：等游戏进程出现后自动注入
#
# 放在游戏目录（与 Ra3Copilot-win/ 同级）可直接用；放在仓库里时用
#   RA3_DIR="/path/to/Red Alert 3" tools/linux/ra3copilot.sh inject
set -euo pipefail

SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
GAME_DIR="${RA3_DIR:-$(cd "$(dirname "$0")" && pwd)}"
DIR="$GAME_DIR/Ra3Copilot-win"
[ -f "$DIR/injector.exe" ] || { echo "缺少 $DIR/injector.exe（用 RA3_DIR 指向游戏目录）"; exit 1; }

cmd="${1:-inject}"

if [ "$cmd" = "ping" ]; then
  curl -sS -m 3 http://127.0.0.1:17832/ping && echo || { echo "接口无应答（DLL 未注入或游戏未进对局）"; exit 1; }
  exit 0
fi

find_game_pid() {
  local d
  for d in /proc/[0-9]*; do
    [ -r "$d/comm" ] || continue
    [ "$(cat "$d/comm" 2>/dev/null)" = "RA3_1.12.game" ] || continue
    grep -q '^State:[[:space:]]*Z' "$d/status" 2>/dev/null && continue
    echo "${d#/proc/}"
    return 0
  done
  return 1
}

# Steam 启动项写成：  "<abs>/ra3copilot.sh" launch %command%
# 先原样拉起游戏，同时后台等进程出现再注入（DLL 自己会等 d3d9.dll，早注入是安全的）。
if [ "$cmd" = "launch" ]; then
  shift
  [ "$#" -gt 0 ] || { echo "用法: $0 launch <命令...>（Steam 启动项写 ... launch %command%）"; exit 1; }
  (
    pid=""
    for _ in $(seq 1 "${RA3_WAIT_SECS:-180}"); do
      pid="$(find_game_pid || true)"
      if [ -n "$pid" ]; then break; fi   # set -e 下不能用 `[ ] && break`（最后一命令失败会退出子 shell）
      sleep 1
    done
    [ -n "$pid" ] || { echo "等待 RA3_1.12.game 超时"; exit 1; }
    sleep 2
    "$SELF" inject
  ) >"$DIR/inject-launch.log" 2>&1 &
  exec "$@"
fi

pid="$(find_game_pid || true)"
[ -n "$pid" ] || { echo "没找到运行中的 RA3_1.12.game（先启动游戏并进入对局）"; exit 1; }

WINEPREFIX="$(tr '\0' '\n' < "/proc/$pid/environ" | sed -n 's/^WINEPREFIX=//p' | head -1)"
TOOLS="$(tr '\0' '\n' < "/proc/$pid/environ" | sed -n 's/^STEAM_COMPAT_TOOL_PATHS=//p' | head -1)"
OVR="$(tr '\0' '\n' < "/proc/$pid/environ" | sed -n 's/^WINEDLLOVERRIDES=//p' | head -1)"
# 冒号列表里可能先出现 runtime，再出现 Proton；取第一个真有 wine 的目录
WINE=""
oldifs="$IFS"; IFS=':'
for p in $TOOLS; do
  if [ -x "$p/files/bin/wine" ]; then WINE="$p"; break; fi
done
IFS="$oldifs"
WINE="${WINE:-$HOME/.local/share/Steam/compatibilitytools.d/Proton-GE Latest}"
[ -x "$WINE/files/bin/wine" ] || { echo "找不到 Proton wine: $WINE"; exit 1; }

echo "游戏 pid : $pid"
echo "prefix   : $WINEPREFIX"
echo "wine     : $WINE/files/bin/wine"
echo "wine ver : $("$WINE/files/bin/wine" --version 2>/dev/null || echo '?')"

args=()
[ "$cmd" = "unload" ] && args+=(--unload)
[ "$cmd" = "status" ] && args+=(--status)

cd "$DIR"
env WINEPREFIX="$WINEPREFIX" WINEDLLOVERRIDES="${OVR:-}" DISPLAY="${DISPLAY:-:1}" \
    "$WINE/files/bin/wine" ./injector.exe "${args[@]}" 2>&1 | iconv -f GBK -t UTF-8 2>/dev/null || true

if [ "$cmd" = "inject" ]; then
  sleep 1
  curl -sS -m 3 http://127.0.0.1:17832/ping && echo " <- /ping OK" || echo "接口还没应答（可能仍在加载，或 DLL 初始化失败）"
fi
