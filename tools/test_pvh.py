#!/usr/bin/env python3
"""
SukiOS PVH 模块/功能自动化测试驱动器（GRUB 不可用约束下，仅用 QEMU）。

输入方案：QEMU headless 下 monitor `sendkey` 不注入键盘中断，故经 `-serial
unix` 注入字符；内核 INPUT_SERVER 支持串口控制台输入源（SYS_SERIAL_READ），
把串口 ASCII 当作键盘事件转发给 shell。由此无需物理键盘即可完整驱动 shell、
验证 FS/重启/exec 等。

用法：python3 tools/test_pvh.py
依赖：qemu-system-x86_64、python3
"""
import os
import sys
import time
import socket
import subprocess
import select

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(ROOT, "build", "kernel.elf")
DISK = os.path.join(ROOT, "build", "disk.img")
MON_SOCK = "/tmp/sukios_mon.sock"
SER_SOCK = "/tmp/sukios_ser.sock"


class QemuRunner:
    def __init__(self):
        self.proc = None
        self.mon = None
        self.ser = None

    def start(self):
        for p in (MON_SOCK, SER_SOCK):
            if os.path.exists(p):
                os.remove(p)
        # 先建 serial server，qemu 连接后立刻 accept，避免启动期输出丢弃
        ser_srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        ser_srv.bind(SER_SOCK)
        ser_srv.listen(1)
        mon_srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        mon_srv.bind(MON_SOCK)
        mon_srv.listen(1)

        cmd = [
            "qemu-system-x86_64", "-machine", "pc", "-cpu", "qemu64",
            "-smp", "4", "-m", "2G", "-no-shutdown", "-display", "none",
            "-kernel", KERNEL, "-append", "pvh",
            "-drive", f"file={DISK},format=raw,index=0,media=disk",
            "-serial", f"unix:{SER_SOCK},server,nowait",
            "-monitor", f"unix:{MON_SOCK},server,nowait",
        ]
        self.proc = subprocess.Popen(cmd, preexec_fn=os.setsid,
                                     stderr=subprocess.PIPE)
        self.ser, _ = ser_srv.accept()
        self.ser.settimeout(0.3)
        self.mon, _ = mon_srv.accept()
        self.mon.settimeout(2)
        print("[test] QEMU pid=%d, serial+monitor 已连接" % self.proc.pid)

    def send_line(self, s):
        self.ser.sendall((s + "\n").encode())
        time.sleep(0.4)

    def read_until_prompt(self, timeout=8, tag=""):
        end = time.time() + timeout
        buf = b""
        nrecv = 0
        while time.time() < end:
            r, _, _ = select.select([self.ser], [], [], 0.3)
            if r:
                chunk = self.ser.recv(4096)
                if chunk:
                    nrecv += len(chunk)
                    buf += chunk
                    if buf.rstrip().endswith(b"SukiOS>"):
                        time.sleep(0.4)
                        try:
                            extra, _, _ = select.select([self.ser], [], [], 0.5)
                            if extra:
                                buf += self.ser.recv(8192)
                        except Exception:
                            pass
                        break
        print("    [io] %s recv=%d bytes, prompt=%s" %
              (tag, nrecv, "yes" if buf.rstrip().endswith(b"SukiOS>") else "no"))
        return buf.decode(errors="replace")

    def stop(self):
        try:
            self.mon.sendall(b"quit\n")
        except OSError:
            pass
        try:
            self.proc.terminate()
        except Exception:
            pass


def main():
    import atexit
    if not os.path.exists(KERNEL) or not os.path.exists(DISK):
        print("ERR: 请先 make iso/disk"); sys.exit(1)

    runner = QemuRunner()
    atexit.register(lambda: runner.stop())
    runner.start()
    print("[test] QEMU 启动，等待 shell 提示符 ...")
    first = runner.read_until_prompt(timeout=30, tag="boot")
    if runner.proc.poll() is not None:
        err = runner.proc.stderr.read().decode(errors="replace")
        print("FAIL: QEMU 提前退出，stderr=\n" + err[-1500:])
        sys.exit(1)
    if "SukiOS>" not in first:
        print("FAIL: 30s 内未出现 shell 提示符")
        print("--- serial 已收内容 ---\n" + first[-2500:])
        sys.exit(1)
    print("PASS: 引导 + shell 就绪")

    results = []
    def run_case(name, cmd, expect, neg=None):
        print("[case] >>> %s" % cmd)
        runner.send_line(cmd)
        out = runner.read_until_prompt(timeout=10, tag=cmd)
        passed = expect in out
        if neg:
            passed = passed and (neg not in out)
        results.append((name, passed))
        print(("PASS " if passed else "FAIL ") + name)
        if not passed:
            print("   --- output tail ---\n" + out[-800:])

    run_case("shell help", "help", "Built-in commands")
    run_case("fs ls root", "ls", "README.TXT")
    run_case("fs cat README.TXT", "cat README.TXT", "OK", neg="file not found")
    run_case("fs write TEST.TXT", "write TEST.TXT helloworld", "ok")
    run_case("fs cat TEST.TXT", "cat TEST.TXT", "helloworld")
    run_case("fs rm TEST.TXT", "rm TEST.TXT", "ok")
    run_case("fs exec hello", "exec hello", "hello", neg="spawn fail")

    # 重启：发 reboot，应在 ~2s 内系统重启回到 boot 提示
    print("[case] >>> reboot")
    runner.send_line("reboot")
    time.sleep(4)
    reb = runner.read_until_prompt(timeout=20, tag="reboot")
    reboot_ok = ("SukiOS>" in reb) or ("[boot] SukiOS" in reb)
    results.append(("reboot (8042 pulse, re-boot)", reboot_ok))
    print(("PASS " if reboot_ok else "FAIL ") + "reboot (8042 pulse, re-boot)")

    passed = sum(1 for _, p in results if p)
    total = len(results)
    print("\n==== 功能测试结果 %d/%d ====" % (passed, total))
    for n, p in results:
        print(("  [OK] " if p else "  [XX] ") + n)
    runner.stop()
    sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
    main()
