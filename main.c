#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <psputility.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <string.h>

PSP_MODULE_INFO("NetDialogTest", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

// GU描画用のディスプレイリストとバッファ定義
static unsigned int __attribute__((aligned(16))) list[262144];
#define BUF_WIDTH (512)
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)
#define PIXEL_SIZE (4)
#define FRAME_SIZE (BUF_WIDTH * SCR_HEIGHT * PIXEL_SIZE)

// ホームボタン終了コールバック
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

// 画面描画（GU）の初期化（ダイアログ表示に必須）
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

// ネットワークモジュールのロードと初期化
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

// Wi-Fi接続ダイアログの表示ループ
void show_net_dialog(void) {
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

    if (sceUtilityNetconfInitStart(&data) < 0) return;

    int done = 0;
    while (!done) {
        sceGuStart(GU_DIRECT, list);
        sceGuClearColor(0xFF222222); // 背景をダークグレーで塗りつぶし
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
}

int main(void) {
    setup_callbacks();
    init_gu();

    if (init_network() == 0) {
        show_net_dialog();
    }

    // ダイアログ終了後、少し待ってから終了
    sceKernelDelayThread(1000 * 1000);
    sceKernelExitGame();
    return 0;
}
