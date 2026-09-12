/*
 * user/apps/curl_test.c
 * -----------------------------------------------------------------------------
 * libcurl 移植端到端验证程序（开机自检由 kmain 内嵌 spawn）。
 *
 * 用 libcurl 的 easy 接口对 QEMU user-net 网关（10.0.2.2 = 宿主机）发起真实
 * HTTP GET，验证「libcurl → socket(150-166) → 内核 sys_net_dispatch → net_server
 * (lwIP) → 真实 TCP」整条链路。默认 URL 指向宿主机 8000 端口的 HTTP 服务。
 *
 * 输出经 u_print 落到 serial，供无头回归判读。
 */
#include "lib/suki.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <curl/curl.h>

#define MAXBODY 4096

static char   g_body[MAXBODY];
static size_t g_len;

/* libcurl 写回调：累积响应体（必须返回原始字节数，否则 curl 认为写失败）。 */
static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)userdata;
    size_t n = size * nmemb;
    if (g_len + n > MAXBODY - 1)
        n = (MAXBODY - 1) - g_len;
    memcpy(g_body + g_len, ptr, n);
    g_len += n;
    return size * nmemb;
}

static void pd(uint64_t v)
{
    char b[24];
    u_print(u_utoa_s(v, b, sizeof(b)));
}

int main(int argc, char **argv)
{
    const char *url = (argc > 1 && argv[1] && argv[1][0])
                        ? argv[1] : "http://10.0.2.2:8000/hello.txt";

    u_print("[curl] === libcurl easy GET ===\n");
    u_print("[curl] URL: "); u_print(url); u_print("\n");
    u_print("[curl] libcurl version: "); u_print(curl_version()); u_print("\n");

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        u_print("[curl] curl_global_init FAIL\n");
        sys_exit(1);
    }

    CURLcode rc = CURLE_OK;
    long code = 0;

    /* 网络（DHCP）尚未就绪时 connect 会失败。最多重试 400 次、每次小睡 20ms
     * （共约 8s 预算），既等待 DHCP 绑定、又避免与 net_server 抢 CPU（互相饿死）；
     * 仅在每 20 次打印一次，避免刷屏。 */
    for (int attempt = 0; attempt < 400; attempt++) {
        CURL *h = curl_easy_init();
        if (!h) { u_print("[curl] curl_easy_init FAIL\n"); curl_global_cleanup(); sys_exit(1); }

        g_len = 0;
        g_body[0] = '\0';

        curl_easy_setopt(h, CURLOPT_URL, url);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, NULL);
        curl_easy_setopt(h, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);

        rc = curl_easy_perform(h);
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(h);

        if (rc == CURLE_OK)
            break;
        if ((attempt % 20) == 0) {
            u_print("[curl] attempt "); pd((uint64_t)attempt);
            u_print(" rc="); pd((uint64_t)rc);
            u_print(" ("); u_print(curl_easy_strerror(rc)); u_print("), retry\n");
        }
        struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 20 * 1000 * 1000;
        nanosleep(&ts, (struct timespec *)0);
    }

    if (rc == CURLE_OK) {
        u_print("[curl] HTTP status="); pd((uint64_t)code);
        u_print(" body="); pd((uint64_t)g_len); u_print(" bytes\n");
        u_print("[curl] body: "); u_print(g_body); u_print("\n");
        u_print("[curl] libcurl HTTP GET: PASS\n");
    } else {
        u_print("[curl] libcurl HTTP GET: FAIL rc="); pd((uint64_t)rc);
        u_print(" ("); u_print(curl_easy_strerror(rc)); u_print(")\n");
    }

    curl_global_cleanup();
    sys_exit(rc == CURLE_OK ? 0 : 1);
    return 0;
}
