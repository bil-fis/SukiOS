/*
 * user/apps/nettest.c
 * -----------------------------------------------------------------------------
 * 网络子系统端到端验证程序（由 Shell 经 ::BIN/NETTEST 启动）。
 *
 * 覆盖两条平行接口：
 *   1) POSIX 网络 syscall（150..164）的确定性往返验证：
 *      - 向 QEMU 内置 TFTP 服务器（10.0.2.2:69）发一个 RRQ（读请求）。slirp 的 TFTP
 *        服务必然回送一个 UDP 包（DATA 或 ERROR），从而确定性地验证
 *        「内核转发 NS_PORT -> net_server(lwIP) -> 真实 UDP 收发（接收路径）」。
 *      - 若 TFTP 根目录未配置，会回 ERROR 包，仍证明 recv 路径打通。
 *   2) SukiNative 原生 socket 对象：SukiSocketCreate/close 冒烟测试。
 *
 * 输出经 u_print 落到 serial，供 QEMU 启动日志核验。
 */
#include "lib/suki.h"
#include <string.h>
#include <sukios/posix.h>
#include <sukios/net.h>

static void pd(uint64_t v)
{
    char b[24];
    u_print(u_utoa_s(v, b, sizeof(b)));
}

static int sys_socket(int d, int t, int p)
{
    return (int)suki_syscall5(SYS_SOCKET, (uint64_t)d, (uint64_t)t, (uint64_t)p, 0, 0);
}
static int sys_bind(int fd, void *a, int al)
{
    return (int)suki_syscall5(SYS_BIND, (uint64_t)fd, (uint64_t)a, (uint64_t)al, 0, 0);
}
static int sys_sendto(int fd, const void *b, size_t l, int f, void *a, int al)
{
    return (int)suki_syscall6(SYS_SENDTO, (uint64_t)fd, (uint64_t)b, (uint64_t)l,
                              (uint64_t)f, (uint64_t)a, (uint64_t)al);
}
static int sys_recvfrom(int fd, void *b, size_t l, int f, void *a, int *al)
{
    return (int)suki_syscall6(SYS_RECVFROM, (uint64_t)fd, (uint64_t)b, (uint64_t)l,
                              (uint64_t)f, (uint64_t)a, (uint64_t)al);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    u_print("[nettest] === POSIX socket(UDP) -> QEMU TFTP 10.0.2.2:69 (RX proof) ===\n");

    int fd = sys_socket(SUKI_AF_INET, SUKI_SOCK_DGRAM, 0);
    if (fd < 0) { u_print("[nettest] socket() FAIL rc="); pd((uint64_t)fd); u_print("\n"); sys_exit(1); }
    u_print("[nettest] socket() ok fd="); pd((uint64_t)fd); u_print("\n");

    /* 构造 TFTP RRQ：opcode=1, filename="nettest.txt", mode="octet" */
    uint8_t rrq[64];
    int rl = 0;
    rrq[rl++] = 0; rrq[rl++] = 1;
    const char *fn = "nettest.txt";
    for (int i = 0; fn[i]; i++) rrq[rl++] = (uint8_t)fn[i];
    rrq[rl++] = 0;
    const char *mode = "octet";
    for (int i = 0; mode[i]; i++) rrq[rl++] = (uint8_t)mode[i];
    rrq[rl++] = 0;

    suki_sockaddr_in_t tftp;
    memset(&tftp, 0, sizeof(tftp));
    tftp.sin_family = SUKI_AF_INET;
    tftp.sin_port   = 69;                 /* TFTP, host order */
    tftp.sin_addr[0]=10; tftp.sin_addr[1]=0; tftp.sin_addr[2]=2; tftp.sin_addr[3]=2;

    /* UDP send 在 DHCP 尚未完成（netif 还没有源 IP）时会因无路由返回 -EIO
     * （ERR_RTE）。net_server 开机即发起 DHCP，约数百毫秒内 BOUND；这里重试至多
     * 50 次并每次让出 CPU，确保 DHCP 完成后再真正发出（避免时序竞态导致 recvfrom
     * 永不到达）。这与真实客户端「先等网络就绪再发」等价。 */
    int nw = -1;
    for (int attempt = 0; attempt < 50; attempt++) {
        nw = sys_sendto(fd, rrq, (size_t)rl, 0, &tftp, (int)sizeof(tftp));
        if (nw >= 0) break;
        u_print("[nettest] sendto retry "); pd((uint64_t)attempt);
        u_print(" (rc="); pd((uint64_t)nw); u_print(")\n");
        suki_syscall1(SYS_YIELD, 0);
    }
    u_print("[nettest] sendto(TFTP RRQ) nwritten="); pd((uint64_t)nw); u_print("\n");
    if (nw < 0) { u_print("[nettest] sendto() FAIL\n"); sys_exit(2); }

    uint8_t rbuf[512];
    suki_sockaddr_in_t from;
    int fromlen = (int)sizeof(from);
    memset(&from, 0, sizeof(from));
    u_print("[nettest] recvfrom() blocking for TFTP reply...\n");
    int nr = sys_recvfrom(fd, rbuf, sizeof(rbuf), 0, &from, &fromlen);
    if (nr < 0) {
        u_print("[nettest] recvfrom() FAIL rc="); pd((uint64_t)nr); u_print("\n");
        sys_exit(3);
    }
    u_print("[nettest] recvfrom() got "); pd((uint64_t)nr);
    u_print(" bytes from ");
    pd((uint64_t)from.sin_addr[0]); u_print(".");
    pd((uint64_t)from.sin_addr[1]); u_print(".");
    pd((uint64_t)from.sin_addr[2]); u_print(".");
    pd((uint64_t)from.sin_addr[3]); u_print(":"); pd((uint64_t)from.sin_port);
    u_print("\n");
    if (nr >= 2 && rbuf[1] == 5) {
        u_print("[nettest] TFTP ERROR reply received (recv path OK)\n");
    } else if (nr >= 2) {
        u_print("[nettest] TFTP reply opcode="); pd((uint64_t)rbuf[1]); u_print("\n");
    }
    u_print("[nettest] POSIX UDP round-trip (with reply): PASS\n");

    /* ---- SukiNative 原生 socket 冒烟 ---- */
    u_print("[nettest] === SukiNative SukiSocketCreate(DGRAM) ===\n");
    suki_socket_t sk;
    suki_status_t st = SukiSocketCreate(SUKI_AF_INET, SUKI_SOCK_DGRAM, 0, &sk);
    if (st != 0) {
        u_print("[nettest] SukiSocketCreate FAIL st="); pd((uint64_t)st); u_print("\n");
    } else {
        u_print("[nettest] SukiSocketCreate ok handle="); pd((uint64_t)sk); u_print("\n");
        suki_status_t cs = SukiSocketClose(sk);
        u_print("[nettest] SukiSocketClose st="); pd((uint64_t)cs); u_print("\n");
    }

    u_print("[nettest] all done.\n");
    sys_exit(0);
    return 0;
}
