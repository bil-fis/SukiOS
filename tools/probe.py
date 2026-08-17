#!/usr/bin/env python3
"""最小探针：启动 QEMU(PVH)，accept serial，读 15s，打印字节数后退出。"""
import os, sys, time, socket, subprocess, select

ROOT = "/mnt/d/Projects/SukiOS"
KERNEL = os.path.join(ROOT, "build", "kernel.elf")
DISK = os.path.join(ROOT, "build", "disk.img")
MON = "/tmp/probe_mon.sock"
SER = "/tmp/probe_ser.sock"
for p in (MON, SER):
    if os.path.exists(p):
        os.remove(p)

ser_srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); ser_srv.bind(SER); ser_srv.listen(1)
mon_srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); mon_srv.bind(MON); mon_srv.listen(1)

cmd = ["qemu-system-x86_64", "-machine", "pc", "-cpu", "qemu64", "-smp", "4", "-m", "2G",
       "-no-shutdown", "-display", "none", "-kernel", KERNEL, "-append", "pvh",
       "-drive", f"file={DISK},format=raw,index=0,media=disk",
       "-serial", f"unix:{SER},server,nowait", "-monitor", f"unix:{MON},server,nowait"]
print("[probe] launching qemu ...", flush=True)
proc = subprocess.Popen(cmd, preexec_fn=os.setsid, stderr=subprocess.PIPE)
print("[probe] qemu pid=%d, waiting accept" % proc.pid, flush=True)
ser, _ = ser_srv.accept(); ser.settimeout(0.3)
mon, _ = mon_srv.accept(); mon.settimeout(2)
print("[probe] serial+monitor accepted", flush=True)

buf = b""
end = time.time() + 15
while time.time() < end:
    r, _, _ = select.select([ser], [], [], 0.3)
    if r:
        c = ser.recv(4096)
        if c:
            buf += c
print("[probe] received %d bytes in 15s" % len(buf), flush=True)
print("[probe] --- first 1200 bytes ---", flush=True)
print(buf[:1200].decode(errors="replace"), flush=True)
if proc.poll() is None:
    mon.sendall(b"quit\n")
    time.sleep(1)
    try: proc.terminate()
    except Exception: pass
print("[probe] done", flush=True)
