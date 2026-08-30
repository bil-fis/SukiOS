#!/usr/bin/env bash
# SukiOS 分层启动复现驱动（纯 bash + QEMU 自带机制，遵守项目调试红线）。
# 用法: ./tools/layer_test.sh <TEST_LAYER> <ATA_PROBE> <DISK_SRV> [秒数]
#  例: ./tools/layer_test.sh 0 0 0    # L0 基线
#      ./tools/layer_test.sh 1 1 0    # L1a: 仅 ata_init
#      ./tools/layer_test.sh 1 1 1    # L1b: + disk-srv
set -u
TL="${1:-0}"; AP="${2:-0}"; DS="${3:-0}"; SECS="${4:-8}"
ROOT="/mnt/d/Projects/SukiOS"
KMAIN="$ROOT/kernel/kmain.c"
LOG="$ROOT/build/layer_${TL}_${AP}_${DS}.log"
INTLOG="$ROOT/build/layer_${TL}_${AP}_${DS}.int.log"

# 1) 注入分层宏（保留 #ifndef 防护，故用 sed 改 #define 行）
sed -i -E "s/^#define TEST_LAYER .*/#define TEST_LAYER $TL/" "$KMAIN"
sed -i -E "s/^#define ATA_PROBE_ENABLE .*/#define ATA_PROBE_ENABLE $AP/" "$KMAIN"
sed -i -E "s/^#define DISK_SRV_ENABLE .*/#define DISK_SRV_ENABLE $DS/" "$KMAIN"
echo "[test] TL=$TL AP=$AP DS=$DS  -> $LOG"

# 2) 仅重建受影响的对象（kmain.o + 重链 kernel.elf），跳过 ISO/disk
cd "$ROOT"
make build/kernel.ski >/tmp/sukios_make.log 2>&1 || { echo "[test] MAKE FAILED"; tail -30 /tmp/sukios_make.log; exit 1; }

# 3) PVH 直启：串口落盘 + -d int 捕获异常 + -no-reboot（避免重启掩盖卡死）
rm -f "$LOG" "$INTLOG"
timeout "$SECS" qemu-system-x86_64 \
  -machine pc,accel=kvm -cpu host -smp 1 -m 2G -no-shutdown \
  -display none -serial file:"$LOG" \
  -kernel build/kernel.ski -append "pvh" \
  -drive file=build/disk.img,format=raw,index=0,media=disk \
  -d int -D "$INTLOG" -no-reboot \
  >/dev/null 2>&1
echo "[test] qemu exit (or timeout after ${SECS}s)"

# 4) 判读结果（不依赖 python，纯 grep/awk）
echo "================ SERIAL TAIL ================"
tail -25 "$LOG" | cat -v
echo "================ EXCEPTION SCAN ============="
grep -E "#GP|#PF|vector 6|system halted| Triple|triple|panic" "$LOG" | head -20
echo "================ QEMU INT LOG (last 15) ====="
grep -E "v=.*a=.*e=" "$INTLOG" | tail -15
echo "================ VERDICT ============="
if grep -q "system fully up\|all services spawned\|kernel baseline only\|disk-srv ready\|L1a: storage probe done" "$LOG"; then
  echo "RESULT: REACHED-LAYER-GOAL (stable to this layer)"
elif grep -q "system halted\|panic\|vector 6" "$LOG"; then
  echo "RESULT: CRASH"
else
  echo "RESULT: UNKNOWN/POSSIBLY-FROZEN (no layer-goal marker; check tail)"
fi
