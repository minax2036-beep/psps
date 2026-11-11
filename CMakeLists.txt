cmake_minimum_required(VERSION 3.10)
project(NetDialogTest C)

add_executable(${PROJECT_NAME} main.c)

# Wi-Fi・ネットワーク・ダイアログ・GU描画に必要なライブラリ群
target_link_libraries(${PROJECT_NAME}
    pspgu
    pspdisplay
    pspge
    psputility
    pspnet
    pspnet_inet
    pspnet_apctl
    pspnet_resolver
    pspdebug
)

# EBOOT.PBPを生成するマクロ
create_pbp_file(
    TARGET ${PROJECT_NAME}
    TITLE "Wi-Fi Dialog Test"
)
