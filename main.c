#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <psputility.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <pspdebug.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

PSP_MODULE_INFO("PSPStore", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(12 * 1024);

static unsigned int *list = NULL;
#define BUF_WIDTH (512)
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)
#define PIXEL_SIZE (4)
#define FRAME_SIZE (BUF_WIDTH * SCR_HEIGHT * PIXEL_SIZE)

#define printf pspDebugScreenPrintf

int exit_callback(int arg1, int arg2, void *common) {
    sceKernelExitGame();
    return 0;
}

int CallbackThread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

void setup_callbacks(void) {
    int thid = sceKernelCreateThread("update_thread", CallbackThread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) sceKernelStartThread(thid, 0, 0);
}

void init_gu(void) {
    if (!list) {
        list = (unsigned int *)memalign(16, 262144);
    }
    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void*)0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void*)FRAME_SIZE, BUF_WIDTH);
    sceGuDepthBuffer((void*)(FRAME_SIZE * 2), BUF_WIDTH);
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

int init_network(void) {
    int res;
    res = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if (res < 0) return res;
    res = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    if (res < 0) return res;

    res = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
    if (res < 0) return res;
    res = sceNetInetInit();
    if (res < 0) return res;
    res = sceNetResolverInit();
    if (res < 0) return res;
    res = sceNetApctlInit(0x8000, 48);
    if (res < 0) return res;

    return 0;
}

int show_net_dialog(int *init_err) {
    pspUtilityNetconfData data;
    memset(&data, 0, sizeof(data));

    data.base.size = sizeof(data);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &data.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &data.base.buttonSwap);
    data.base.graphicsThread = 17;
    data.base.accessThread = 19;
    data.base.fontThread = 18;
    data.base.soundThread = 16;
    data.action = PSP_NETCONF_ACTION_CONNECTAP;

    int res = sceUtilityNetconfInitStart(&data);
    if (res < 0) {
        *init_err = res;
        return -1;
    }

    int done = 0;
    while (!done) {
        sceGuStart(GU_DIRECT, list);
        sceGuClearColor(0xFF222222);
        sceGuClear(GU_COLOR_BUFFER_BIT);
        sceGuFinish();
        sceGuSync(0, 0);

        switch (sceUtilityNetconfGetStatus()) {
            case PSP_UTILITY_DIALOG_VISIBLE:
                sceUtilityNetconfUpdate(1);
                break;
            case PSP_UTILITY_DIALOG_QUIT:
                sceUtilityNetconfShutdownStart();
                break;
            case PSP_UTILITY_DIALOG_NONE:
                done = 1;
                break;
            default:
                break;
        }
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }
    *init_err = data.base.result;
    return data.base.result;
}

// PSPネイティブDNSリゾルバ（struct in_addr を直接返す）
int resolve_hostname_addr(const char *hostname, struct in_addr *out_addr) {
    int rid = -1;
    void *buf = malloc(1024);
    if (!buf) return -1;

    int res = sceNetResolverCreate(&rid, buf, 1024);
    if (res < 0) {
        free(buf);
        return res;
    }

    memset(out_addr, 0, sizeof(struct in_addr));
    res = sceNetResolverStartNtoA(rid, hostname, out_addr, 2, 3);
    sceNetResolverDelete(rid);
    free(buf);

    return res;
}

// mbedTLS用のPSPネイティブ送信コールバック
int psp_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int sock = *((int *)ctx);
    int ret = sceNetInetSend(sock, buf, len, 0);
    if (ret < 0) {
        int err = sceNetInetGetErrno();
        if (err == EAGAIN || err == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return ret;
}

// mbedTLS用のPSPネイティブ受信コールバック
int psp_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int sock = *((int *)ctx);
    int ret = sceNetInetRecv(sock, buf, len, 0);
    if (ret < 0) {
        int err = sceNetInetGetErrno();
        if (err == EAGAIN || err == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return ret;
}

void fetch_github_tls(void) {
    char *server_name = (char *)calloc(1, 128);
    char *request = (char *)calloc(1, 512);
    if (!server_name || !request) return;

    sprintf(server_name, "%s.%s.%s", "raw", "githubusercontent", "com");

    sprintf(request,
        "GET /PSP-Archive/ARK-4/main/README.md HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: PSP-Store/1.0\r\n"
        "Connection: close\r\n\r\n", server_name);

    struct in_addr server_addr;
    printf("Resolving DNS for [%s]...\n", server_name);
    int dns_res = resolve_hostname_addr(server_name, &server_addr);
    if (dns_res < 0) {
        printf("ERROR: DNS failed (0x%08X)\n", dns_res);
        free(server_name);
        free(request);
        return;
    }

    unsigned char *b = (unsigned char *)&server_addr.s_addr;
    printf("Resolved IP: %u.%u.%u.%u\n", *b, *(b + 1), *(b + 2), *(b + 3));

    // PSPネイティブソケットを作成して443番ポートへ直接TCP接続
    int sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("ERROR: Socket create failed (0x%08X)\n", sceNetInetGetErrno());
        free(server_name);
        free(request);
        return;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(443);
    sa.sin_addr = server_addr;

    printf("Connecting TCP port 443...\n");
    if (sceNetInetConnect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        printf("ERROR: TCP connect failed (0x%08X)\n", sceNetInetGetErrno());
        sceNetInetClose(sock);
        free(server_name);
        free(request);
        return;
    }
    printf("TCP Connected! Starting TLS 1.3 handshake...\n");

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    const char *pers = "psp_store_tls";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0) {
        printf("ERROR: ctr_drbg_seed failed\n");
        goto cleanup;
    }

    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        printf("ERROR: ssl_config_defaults failed\n");
        goto cleanup;
    }

    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

    if (mbedtls_ssl_setup(&ssl, &conf) != 0) {
        printf("ERROR: ssl_setup failed\n");
        goto cleanup;
    }

    mbedtls_ssl_set_hostname(&ssl, server_name);
    // PSPネイティブ送受信関数をmbedTLSに直結！
    mbedtls_ssl_set_bio(&ssl, &sock, psp_bio_send, psp_bio_recv, NULL);

    int ret;
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            printf("ERROR: handshake failed (-0x%x)\n", -ret);
            goto cleanup;
        }
    }
    printf("SUCCESS! Protocol: %s\n", mbedtls_ssl_get_version(&ssl));

    mbedtls_ssl_write(&ssl, (const unsigned char *)request, strlen(request));

    unsigned char *buf = (unsigned char *)calloc(1, 512);
    if (buf) {
        ret = mbedtls_ssl_read(&ssl, buf, 511);
        if (ret > 0) {
            printf("Response:\n%s\n", buf);
        } else {
            printf("Read error or empty: -0x%x\n", -ret);
        }
        free(buf);
    }

    mbedtls_ssl_close_notify(&ssl);

cleanup:
    sceNetInetClose(sock);
    free(server_name);
    free(request);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
}

int main(void) {
    setup_callbacks();
    init_gu();

    int net_err = init_network();
    int dialog_err = 0;
    int dialog_res = -1;

    if (net_err == 0) {
        dialog_res = show_net_dialog(&dialog_err);
    }

    sceGuTerm();
    pspDebugScreenInit();
    printf("PSP Store [v7] - Universal-Updater style\n\n");

    if (net_err != 0) {
        printf("Network Init Error: 0x%08X\n", net_err);
    } else if (dialog_res == 0) {
        printf("Wi-Fi Connected! Starting TLS test...\n");
        fetch_github_tls();
    } else {
        printf("Wi-Fi Dialog Error/Canceled: 0x%08X\n", dialog_err);
    }

    printf("\nPress HOME to exit.\n");
    while (1) {
        sceKernelDelayThread(100 * 1000);
    }
    return 0;
}
