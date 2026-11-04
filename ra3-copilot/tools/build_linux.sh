#!/usr/bin/env bash
# Linux-native cross build for ra3copilot.dll (i686-w64-mingw32).
#
# Requirements: llvm-mingw (ucrt, i686 target) with llvm-ml, clang, lld.
#   download: https://github.com/mstorsjo/llvm-mingw/releases
#   export TOOLCHAIN=~/tools/llvm-mingw-...-ucrt-ubuntu-22.04-x86_64
#
# Deviations from build.bat (MSVC):
#   * MASM stubs: llvm-ml does not accept EXTERNDEF and does not decorate C
#     symbols with '_' -> tools/fix_stubs.py rewrites them (EXTERN + '_').
#   * __try/__except: clang for i686 COFF cannot emit MSVC SEH tables, so the
#     build defines them as no-op macros (crash guards are compiled out).
#   * _ReturnAddress(): clang lowers it to __ReturnAddress, which mingw lacks;
#     we provide a 4-byte stub (tools/retaddr.asm).
#   * NDEBUG: ImGui IM_ASSERT is disabled (a mismatch under DXVK otherwise
#     pops an assertion dialog inside the game).
set -euo pipefail
cd "$(dirname "$0")/.."

T=${TOOLCHAIN:-$HOME/tools/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
B="$T/bin"
CXX="$B/i686-w64-mingw32-clang++"
CC="$B/i686-w64-mingw32-clang"
AS="$B/llvm-ml"
OUT=${OUT:-build_linux}
mkdir -p "$OUT"

INC=(-Iimgui -Iimgui/backends -Isrc -Ithird_party/minhook-1.3.3/include -Ithird_party/minhook-1.3.3/src)
# i686 clang 没有 SEH：__try/__except 在这里被编成空操作。任何"护栏读/写"都必须
# 自带显式 VirtualQuery 校验（见 actions.cpp 的 SafeRead32/IsMemOk），不能只靠 SEH。
FLAGS=(-O2 -DNDEBUG -std=c++17 "-D__try=if(1)" "-D__except(...)=else if(0)"
       -Wno-nontrivial-memcall -Wno-unknown-pragmas ${EXTRA_FLAGS:-})

echo "[1/5] stubs (llvm-ml)"
python3 tools/fix_stubs.py src/stubs.asm "$OUT/stubs.asm"
"$AS" -m32 -filetype=obj -Fo "$OUT/stubs.obj" "$OUT/stubs.asm"

echo "[2/5] MinHook"
for f in buffer hook trampoline; do
  "$CC" -c -O2 "${INC[@]}" -o "$OUT/mh_$f.o" "third_party/minhook-1.3.3/src/$f.c"
done
"$CC" -c -O2 "${INC[@]}" -o "$OUT/mh_hde32.o" third_party/minhook-1.3.3/src/hde/hde32.c

echo "[3/5] ImGui"
for f in imgui imgui_draw imgui_tables imgui_widgets; do
  "$CXX" -c "${FLAGS[@]}" "${INC[@]}" -o "$OUT/$f.o" "imgui/$f.cpp"
done
for f in imgui_impl_dx9 imgui_impl_win32; do
  "$CXX" -c "${FLAGS[@]}" "${INC[@]}" -o "$OUT/$f.o" "imgui/backends/$f.cpp"
done

echo "[4/5] project"
for f in dllmain overlay features actions protocol battlefield aicmd symres hook httpapi envprobe scriptobj scripttable big4 refpack maptbl buildinfo; do
  "$CXX" -c "${FLAGS[@]}" "${INC[@]}" -o "$OUT/$f.o" "src/$f.cpp"
done

echo "[5/5] link"
"$AS" -m32 -filetype=obj -Fo "$OUT/retaddr.obj" tools/retaddr.asm
"$CXX" -shared -o "$OUT/ra3copilot.dll" "$OUT"/*.o "$OUT/stubs.obj" "$OUT/retaddr.obj" \
    -ld3d9 -luser32 -lgdi32 -ldwmapi -lversion -lws2_32 -static-libgcc -static-libstdc++
ls -lh "$OUT/ra3copilot.dll"
