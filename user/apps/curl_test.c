/*
 * user/apps/curl_test.c
 * -----------------------------------------------------------------------------
 * libcurl 移植端到端验证程序（开机自检由 kmain 内嵌 spawn）。
 *
 * 用 libcurl easy 接口对 QEMU user-net 网关（10.0.2.2 = 宿主机）发起真实请求：
 *   1) HTTP  : http://10.0.2.2:8000/hello.txt   （TCP + HTTP）
 *   2) HTTPS : https://10.0.2.2:8443/hello.txt  （TLS，mbedTLS 后端；自签证书，关闭校验）
 * 覆盖「libcurl → socket(150-166) → 内核 sys_net_dispatch → net_server(lwIP) → 真实 TCP」
 * 与「libcurl → mbedTLS(TLS) → 同上」两条链路。输出经 u_print 落到 serial。
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

/*
 * 执行一次 GET（带 DHCP 等待重试）。insecure!=0 时关闭证书校验（自签场景）。
 * 成功返回 0（*out_code 为 HTTP 状态码），否则返回 libcurl 错误码。
 */
static int run_get(const char *tag, const char *url, int insecure, long *out_code)
{
    CURLcode rc = CURLE_OK;
    long code = 0;

    for (int attempt = 0; attempt < 400; attempt++) {
        CURL *h = curl_easy_init();
        if (!h) { u_print("[curl] curl_easy_init FAIL\n"); return -1; }

        g_len = 0;
        g_body[0] = '\0';

        curl_easy_setopt(h, CURLOPT_URL, url);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, NULL);
        curl_easy_setopt(h, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        if (insecure) {
            curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
        }

        u_print("[curl] "); u_print(tag); u_print(" try "); pd((uint64_t)attempt);
        u_print(": perform...\n");
        rc = curl_easy_perform(h);
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(h);
        u_print("[curl] "); u_print(tag); u_print(" try "); pd((uint64_t)attempt);
        u_print(" done rc="); pd((uint64_t)rc); u_print("\n");

        if (rc == CURLE_OK)
            break;
        if ((attempt % 20) == 0) {
            u_print("[curl] "); u_print(tag); u_print(" attempt ");
            pd((uint64_t)attempt); u_print(" rc="); pd((uint64_t)rc);
            u_print(" ("); u_print(curl_easy_strerror(rc)); u_print("), retry\n");
        }
        struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 20 * 1000 * 1000;
        nanosleep(&ts, (struct timespec *)0);
    }

    if (out_code) *out_code = code;
    return (int)rc;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    const char *http_url  = "http://10.0.2.2:8000/hello.txt";
    const char *https_url = "https://10.0.2.2:8443/hello.txt";

    u_print("[curl] === libcurl easy GET ===\n");
    u_print("[curl] libcurl version: "); u_print(curl_version()); u_print("\n");

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        u_print("[curl] curl_global_init FAIL\n");
        sys_exit(1);
    }

    int fails = 0;

    /* ---- 1) HTTP ---- */
    {
        long code = 0;
        int rc = run_get("http", http_url, 0, &code);
        u_print("[curl] HTTP  URL: "); u_print(http_url); u_print("\n");
        if (rc == 0) {
            u_print("[curl] HTTP  status="); pd((uint64_t)code);
            u_print(" body="); pd((uint64_t)g_len); u_print(" bytes: ");
            u_print(g_body); u_print("\n");
            u_print("[curl] libcurl HTTP  GET: PASS\n");
        } else {
            fails++;
            u_print("[curl] libcurl HTTP  GET: FAIL rc="); pd((uint64_t)rc);
            u_print(" ("); u_print(curl_easy_strerror((CURLcode)rc)); u_print(")\n");
        }
    }

    /* ---- 2) HTTPS（TLS via mbedTLS；自签证书，关闭校验） ---- */
    {
        long code = 0;
        int rc = run_get("https", https_url, 1, &code);
        u_print("[curl] HTTPS URL: "); u_print(https_url); u_print("\n");
        if (rc == 0) {
            u_print("[curl] HTTPS status="); pd((uint64_t)code);
            u_print(" body="); pd((uint64_t)g_len); u_print(" bytes: ");
            u_print(g_body); u_print("\n");
            u_print("[curl] libcurl HTTPS GET: PASS\n");
        } else {
            fails++;
            u_print("[curl] libcurl HTTPS GET: FAIL rc="); pd((uint64_t)rc);
            u_print(" ("); u_print(curl_easy_strerror((CURLcode)rc)); u_print(")\n");
        }
    }

    curl_global_cleanup();
    sys_exit(fails == 0 ? 0 : 1);
    return 0;
}
