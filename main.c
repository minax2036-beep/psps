#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <psputility.h>
#include <pspctrl.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
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
#include "font.h"

PSP_MODULE_INFO("PSPStore", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(12 * 1024);

static unsigned int *list = NULL;
#define BUF_WIDTH (512)
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)
#define PIXEL_SIZE (4)
#define FRAME_SIZE (BUF_WIDTH * SCR_HEIGHT * PIXEL_SIZE)

#define COL_BG          0xFF1E1916
#define COL_PANEL       0xFF2B2420
#define COL_HEADER      0xFF382D26
#define COL_ACCENT      0xFFD48828
#define COL_ACCENT_DARK 0xFF7A4E16
#define COL_TEXT        0xFFF5F5F5
#define COL_TEXT_SUB    0xFFB0A8A0
#define COL_GREEN       0xFF55C445
#define COL_BAR_BG      0xFF14110F

typedef struct {
    const char *title;
    const char *author;
    const char *version;
    const char *category;
    const char *desc1;
    const char *desc2;
    const char *path;
} StoreItem;

typedef struct {
    unsigned int color;
    short x, y, z;
} Vertex2D;

void draw_rect(int x, int y, int w, int h, unsigned int color) {
    Vertex2D *v = (Vertex2D *)sceGuGetMemory(2 * sizeof(Vertex2D));
    v->color = color;
    v->x = x;
    v->y = y;
    v->z = 0;
    (v + 1)->color = color;
    (v + 1)->x = x + w;
    (v + 1)->y = y + h;
    (v + 1)->z = 0;
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
}

void draw_border(int x, int y, int w, int h, unsigned int color) {
    draw_rect(x, y, w, 1, color);
    draw_rect(x, y + h - 1, w, 1, color);
    draw_rect(x, y, 1, h, color);
    draw_rect(x + w - 1, y, 1, h, color);
}

void draw_text(int x, int y, const char *str, unsigned int color, int scale) {
    int cx = x;
    while (*str) {
        unsigned char c = (unsigned char)(*str);
        if (c >= 32 && c <= 126) {
            int idx = (c - 32) * 8;
            for (int row = 0; row < 8; row++) {
                unsigned char bits = *(font8x8 + idx + row);
                for (int col = 0; col < 8; col++) {
                    if (bits & (0x80 >> col)) {
                        draw_rect(cx + col * scale, y + row * scale, scale, scale, color);
                    }
                }
            }
        }
        cx += 8 * scale;
        str++;
    }
}

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
    sceGuDisable(GU_TEXTURE_2D);
    sceGuShadeModel(GU_FLAT);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

int init_network(void) {
    if (sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON) < 0) return -1;
    if (sceUtilityLoadNetModule(PSP_NET_MODULE_INET) < 0) return -1;
    if (sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024) < 0) return -1;
    if (sceNetInetInit() < 0) return -1;
    if (sceNetResolverInit() < 0) return -1;
    if (sceNetApctlInit(0x8000, 48) < 0) return -1;
    return 0;
}

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
        sceGuClearColor(COL_BG);
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
void render_store_ui(StoreItem *items, int count, int selected, int wifi_ok,
                     int is_downloading, int progress, const char *status_msg) {
    sceGuStart(GU_DIRECT, list);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuShadeModel(GU_FLAT);
    sceGuClearColor(COL_BG);
    sceGuClear(GU_COLOR_BUFFER_BIT);

    draw_rect(0, 0, SCR_WIDTH, 24, COL_HEADER);
    draw_rect(0, 24, SCR_WIDTH, 2, COL_ACCENT);
    draw_text(10, 8, "PSP Store - Universal-Updater", COL_TEXT, 1);
    if (wifi_ok) {
        draw_text(370, 8, "[Wi-Fi: ON]", COL_GREEN, 1);
    } else {
        draw_text(362, 8, "[Offline]", COL_TEXT_SUB, 1);
    }

    draw_rect(8, 32, 206, 206, COL_PANEL);
    draw_border(8, 32, 206, 206, 0xFF453B35);

    for (int i = 0; i < count; i++) {
        StoreItem *it = items + i;
        int item_y = 38 + i * 32;
        if (i == selected) {
            draw_rect(12, item_y, 198, 28, COL_ACCENT_DARK);
            draw_border(12, item_y, 198, 28, COL_ACCENT);
            draw_rect(12, item_y, 4, 28, COL_ACCENT);
        } else {
            draw_rect(12, item_y, 198, 28, 0xFF231D1A);
        }
        draw_text(20, item_y + 5, it->title, COL_TEXT, 1);
        draw_text(20, item_y + 16, it->author, COL_TEXT_SUB, 1);
    }

    draw_rect(222, 32, 250, 206, COL_PANEL);
    draw_border(222, 32, 250, 206, 0xFF453B35);

    StoreItem *cur = items + selected;
    draw_text(232, 42, cur->title, COL_TEXT, 1);
    draw_rect(232, 56, 230, 1, 0xFF453B35);

    char *meta = (char *)calloc(1, 128);
    if (meta) {
        snprintf(meta, 128, "Ver: %s | By: %s", cur->version, cur->author);
        draw_text(232, 64, meta, COL_ACCENT, 1);
        snprintf(meta, 128, "Category: %s", cur->category);
        draw_text(232, 78, meta, COL_TEXT_SUB, 1);
        free(meta);
    }

    draw_text(232, 100, cur->desc1, COL_TEXT, 1);
    draw_text(232, 114, cur->desc2, COL_TEXT, 1);

    draw_rect(232, 156, 230, 72, COL_BAR_BG);
    draw_border(232, 156, 230, 72, 0xFF3E3530);
    draw_text(240, 164, status_msg, is_downloading ? COL_ACCENT : COL_TEXT_SUB, 1);

    int bar_x = 240;
    int bar_y = 182;
    int bar_w = 214;
    int bar_h = 16;
    draw_rect(bar_x, bar_y, bar_w, bar_h, 0xFF28221E);
    draw_border(bar_x, bar_y, bar_w, bar_h, COL_TEXT_SUB);

    if (progress > 0) {
        int fill_w = (bar_w - 2) * progress / 100;
        if (fill_w > bar_w - 2) fill_w = bar_w - 2;
        draw_rect(bar_x + 1, bar_y + 1, fill_w, bar_h - 2,
                  (progress >= 100) ? COL_GREEN : COL_ACCENT);
    }

    char *pct = (char *)calloc(1, 16);
    if (pct) {
        snprintf(pct, 16, "%d%%", progress);
        draw_text(bar_x + 95, bar_y + 4, pct, COL_TEXT, 1);
        free(pct);
    }

    draw_rect(0, 246, SCR_WIDTH, 26, COL_HEADER);
    draw_rect(0, 245, SCR_WIDTH, 1, 0xFF453B35);
    draw_text(12, 255, "[Up/Down] Select   [X/O] Download   [HOME] Exit", COL_TEXT_SUB, 1);

    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

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
void download_item_with_ui(StoreItem *items, int count, int selected, int wifi_ok, char *out_status) {
    StoreItem *cur = items + selected;
    char *server_name = (char *)calloc(1, 128);
    char *request = (char *)calloc(1, 512);
    if (!server_name || !request) return;

    sprintf(server_name, "%s.%s.%s", "raw", "githubusercontent", "com");
    sprintf(request,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: PSP-Store/1.0\r\n"
        "Connection: close\r\n\r\n", cur->path, server_name);

    render_store_ui(items, count, selected, wifi_ok, 1, 10, "Resolving DNS...");

    struct in_addr server_addr;
    if (resolve_hostname_addr(server_name, &server_addr) < 0) {
        strcpy(out_status, "Error: DNS failed");
        goto end_clean;
    }

    render_store_ui(items, count, selected, wifi_ok, 1, 25, "Connecting TCP 443...");
    int sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(443);
    sa.sin_addr = server_addr;

    if (sceNetInetConnect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        sceNetInetClose(sock);
        strcpy(out_status, "Error: TCP failed");
        goto end_clean;
    }

    render_store_ui(items, count, selected, wifi_ok, 1, 45, "TLS 1.3 Handshake...");

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    const char *pers = "psp_store_tls";
    mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                          (const unsigned char *)pers, strlen(pers));
    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                MBEDTLS_SSL_TRANSPORT_STREAM,
                                MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    mbedtls_ssl_setup(&ssl, &conf);
    mbedtls_ssl_set_hostname(&ssl, server_name);
    mbedtls_ssl_set_bio(&ssl, &sock, psp_bio_send, psp_bio_recv, NULL);

    int ret;
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            strcpy(out_status, "Error: TLS handshake failed");
            goto tls_clean;
        }
    }

    render_store_ui(items, count, selected, wifi_ok, 1, 70, "Downloading data...");
    mbedtls_ssl_write(&ssl, (const unsigned char *)request, strlen(request));

    unsigned char *buf = (unsigned char *)calloc(1, 2048);
    int total_bytes = 0;
    if (buf) {
        while ((ret = mbedtls_ssl_read(&ssl, buf, 2047)) > 0) {
            total_bytes += ret;
            int pct = 70 + (total_bytes / 400);
            if (pct > 98) pct = 98;
            snprintf(out_status, 128, "Received: %d bytes", total_bytes);
            render_store_ui(items, count, selected, wifi_ok, 1, pct, out_status);
        }
        free(buf);
    }

    snprintf(out_status, 128, "Done! (%d bytes)", total_bytes);
    mbedtls_ssl_close_notify(&ssl);

tls_clean:
    sceNetInetClose(sock);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

end_clean:
    free(server_name);
    free(request);
}

int main(void) {
    setup_callbacks();
    init_gu();

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    int wifi_ok = 0;
    if (init_network() == 0) {
        if (show_net_dialog() == 0) {
            wifi_ok = 1;
        }
    }

    StoreItem *items = (StoreItem *)calloc(6, sizeof(StoreItem));
    int item_count = 6;

    (items + 0)->title = "ARK-4 Custom Firmware";
    (items + 0)->author = "PSP-Archive";
    (items + 0)->version = "v4.20.69";
    (items + 0)->category = "CFW / System";
    (items + 0)->desc1 = "Modern PSP Custom Firmware";
    (items + 0)->desc2 = "with WPA2 & VSH enhancements.";
    (items + 0)->path = "/PSP-Archive/ARK-4/main/README.md";

    (items + 1)->title = "ARK-5 Next-Gen CFW";
    (items + 1)->author = "PSP-Arkfive";
    (items + 1)->version = "v5.0-dev";
    (items + 1)->category = "CFW / System";
    (items + 1)->desc1 = "Next-gen CFW featuring VSHGU";
    (items + 1)->desc2 = "and lightweight overlay UI.";
    (items + 1)->path = "/PSP-Archive/ARK-4/main/README.md";

    (items + 2)->title = "Moonlight PSP";
    (items + 2)->author = "k4idyn";
    (items + 2)->version = "v0.9.2";
    (items + 2)->category = "Streaming";
    (items + 2)->desc1 = "Stream PC games to your PSP";
    (items + 2)->desc2 = "via NVIDIA GameStream/Sunshine.";
    (items + 2)->path = "/PSP-Archive/ARK-4/main/README.md";

    (items + 3)->title = "Category Lite";
    (items + 3)->author = "Bubbletune";
    (items + 3)->version = "v1.7";
    (items + 3)->category = "Plugin";
    (items + 3)->desc1 = "Organize your XMB games and";
    (items + 3)->desc2 = "homebrews into clean folders.";
    (items + 3)->path = "/PSP-Archive/ARK-4/main/README.md";

    (items + 4)->title = "CXMB Theme Loader";
    (items + 4)->author = "Poison";
    (items + 4)->version = "v3.71";
    (items + 4)->category = "Plugin";
    (items + 4)->desc1 = "Load custom CTF themes from";
    (items + 4)->desc2 = "Memory Stick without flashing.";
    (items + 4)->path = "/PSP-Archive/ARK-4/main/README.md";

    (items + 5)->title = "PSP Filer";
    (items + 5)->author = "Mediumgauge";
    (items + 5)->version = "v6.6";
    (items + 5)->category = "Utility";
    (items + 5)->desc1 = "All-in-one file manager,";
    (items + 5)->desc2 = "ZIP extractor & hex viewer.";
    (items + 5)->path = "/PSP-Archive/ARK-4/main/README.md";

    int selected = 0;
    int progress = 0;
    char *status_msg = (char *)calloc(1, 128);
    strcpy(status_msg, "Ready. Press [X] or [O] to install.");

    SceCtrlData pad;
    unsigned int old_buttons = 0;

    while (1) {
        sceCtrlReadBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~old_buttons;
        old_buttons = pad.Buttons;

        if (pressed & PSP_CTRL_UP) {
            selected--;
            if (selected < 0) selected = item_count - 1;
            progress = 0;
            strcpy(status_msg, "Ready. Press [X] or [O] to install.");
        }
        if (pressed & PSP_CTRL_DOWN) {
            selected++;
            if (selected >= item_count) selected = 0;
            progress = 0;
            strcpy(status_msg, "Ready. Press [X] or [O] to install.");
        }
        if (pressed & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE)) {
            if (wifi_ok) {
                download_item_with_ui(items, item_count, selected, wifi_ok, status_msg);
                progress = (strncmp(status_msg, "Done", 4) == 0) ? 100 : 0;
                sceCtrlReadBufferPositive(&pad, 1);
                old_buttons = pad.Buttons;
            } else {
                strcpy(status_msg, "Offline! Connect Wi-Fi first.");
            }
        }

        render_store_ui(items, item_count, selected, wifi_ok, 0, progress, status_msg);
    }

    free(status_msg);
    free(items);
    sceKernelExitGame();
    return 0;
}
