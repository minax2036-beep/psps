#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <psputility.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspdebug.h>
#include <psprtc.h>
#include <string.h>
#include <stdio.h>

// mbedTLS ヘッダ
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"

PSP_MODULE_INFO("GitHubTLSTest", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

// メモリ節約のためヒープサイズを12MBに制限（15MB以下を確実に維持）
PSP_HEAP_SIZE_KB(12 * 1024);

static unsigned int __attribute__((aligned(16))) list[262144];
#define BUF_WIDTH (512)
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)
#define PIXEL_SIZE (4)
#define FRAME_SIZE (BUF_WIDTH * SCR_HEIGHT * PIXEL_SIZE)

#define printf pspDebugScreenPrintf

// 終了コールバック
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

// GU初期化（ダイアログ用）
void init_gu(void) {
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

// ネットワーク初期化
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
    res = sceNetApctlInit(0x8000, 48);
    if (res < 0) return res;

    return 0;
}

// Wi-Fiダイアログ表示
int show_net_dialog(void) {
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

    if (sceUtilityNetconfInitStart(&data) < 0) return -1;

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
    return data.base.result;
}

// GitHubへTLS 1.3でHTTPSリクエストを送信する関数
void fetch_github_tls(void) {
    const char *server_name = "://githubusercontent.com";
    const char *server_port = "443";
    // ARK-4の公式リポジトリにある軽量なテキストを取得テスト
    const char *request =
        "GET /PSP-Archive/ARK-4/main/README.md HTTP/1.1\r\n"
        "Host: ://githubusercontent.com\r\n"
        "User-Agent: PSP-TLS-Client/1.0\r\n"
        "Connection: close\r\n\r\n";

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

    printf("[1] Seeding RNG...\n");
    const char *pers = "psp_github_tls";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0) {
        printf("ERROR: ctr_drbg_seed failed\n");
        goto cleanup;
    }

    printf("[2] Connecting to %s:%s...\n", server_name, server_port);
    if (mbedtls_net_connect(&server_fd, server_name, server_port, MBEDTLS_NET_PROTO_TCP) != 0) {
        printf("ERROR: net_connect failed\n");
        goto cleanup;
    }

    printf("[3] Setting up TLS 1.2/1.3 config...\n");
    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        printf("ERROR: ssl_config_defaults failed\n");
        goto cleanup;
    }

    // メモリ節約のため証明書検証をスキップ（後でルート証明書1枚に絞って有効化可能）
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

    if (mbedtls_ssl_setup(&ssl, &conf) != 0) {
        printf("ERROR: ssl_setup failed\n");
        goto cleanup;
    }

    // SNI（Server Name Indication）のセット（GitHub通信には必須！）
    if (mbedtls_ssl_set_hostname(&ssl, server_name) != 0) {
        printf("ERROR: set_hostname failed\n");
        goto cleanup;
    }

    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

    printf("[4] Performing TLS handshake...\n");
    int ret;
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            printf("ERROR: handshake failed (-0x%x)\n", -ret);
            goto cleanup;
        }
    }
    printf("SUCCESS! Connected using: %s\n", mbedtls_ssl_get_version(&ssl));

    printf("[5] Sending HTTPS GET request...\n");
    mbedtls_ssl_write(&ssl, (const unsigned char *)request, strlen(request));

    printf("[6] Reading response (first 512 bytes):\n----------------------------------------\n");
    unsigned char buf[513];
    memset(buf, 0, sizeof(buf));
    ret = mbedtls_ssl_read(&ssl, buf, sizeof(buf) - 1);
    if (ret > 0) {
        printf("%s\n", buf);
    } else {
        printf("Read error or empty: -0x%x\n", -ret);
    }

    mbedtls_ssl_close_notify(&ssl);

cleanup:
    mbedtls_net_free(&server_fd);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
}

int main(void) {
    setup_callbacks();
    init_gu();

    if (init_network() == 0) {
        int res = show_net_dialog();

        // ダイアログ終了後、文字表示用のデバッグスクリーンに切り替え
        sceGuTerm();
        pspDebugScreenInit();

        if (res == 0) {
            printf("Wi-Fi Connected! Starting TLS test...\n\n");
            fetch_github_tls();
        } else {
            printf("Wi-Fi connection canceled or failed.\n");
        }
    }

    printf("\nPress HOME to exit.\n");
    while (1) {
        sceKernelDelayThread(100 * 1000);
    }
    return 0;
}
