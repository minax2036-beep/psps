#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <psputility.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <pspdebug.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

#include "mbedtls/net_sockets.h"
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

int resolve_hostname_psp(const char *hostname, char *ip_str_out, size_t max_len) {
    int rid = -1;
    void *buf = malloc(1024);
    if (!buf) return -1;

    int res = sceNetResolverCreate(&rid, buf, 1024);
    if (res < 0) {
        free(buf);
        return res;
    }

    struct in_addr addr;
    res = sceNetResolverStartNtoA(rid, hostname, &addr, 2, 3);
    sceNetResolverDelete(rid);
    free(buf);

    if (res < 0) return res;

    const char *ip = inet_ntoa(addr);
    if (!ip) return -1;

    strncpy(ip_str_out, ip, max_len - 1);
    ip_str_out[max_len - 1] = '\0';
    return 0;
}

void fetch_github_tls(void) {
    const char *server_name = "://githubusercontent.com";
    const char *server_port = "443";
    const char *request =
        "GET /PSP-Archive/ARK-4/main/README.md HTTP/1.1\r\n"
        "Host: ://githubusercontent.com\r\n"
        "User-Agent: PSP-Store/1.0\r\n"
        "Connection: close\r\n\r\n";

    char *ip_str = (char *)calloc(1, 64);
    if (!ip_str) return;

    printf("Resolving DNS for %s...\n", server_name);
    int dns_res = resolve_hostname_psp(server_name, ip_str, 64);
    if (dns_res < 0) {
        printf("ERROR: DNS failed (0x%08X)\n", dns_res);
        free(ip_str);
        return;
    }
    printf("Resolved IP: %s\n", ip_str);

    mbedtls_net_context server_fd;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;

    mbedtls_net_init(&server_fd);
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

    printf("Connecting TCP to %s:%s...\n", ip_str, server_port);
    int ret = mbedtls_net_connect(&server_fd, ip_str, server_port, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        printf("ERROR: net_connect failed (-0x%x)\n", -ret);
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
    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

    printf("Performing TLS 1.3 handshake...\n");
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
    free(ip_str);
    mbedtls_net_free(&server_fd);
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
    printf("PSP Store [v3] - Universal-Updater style\n\n");

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
