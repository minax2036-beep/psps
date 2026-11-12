cmake_minimum_required(VERSION 3.10)
project(NetDialogTest C)

add_executable(${PROJECT_NAME} main.c font.c)

target_link_libraries(${PROJECT_NAME}
    mbedtls
    mbedx509
    mbedcrypto
    pspgu
    pspdisplay
    pspge
    psputility
    pspctrl
    pspnet
    pspnet_inet
    pspnet_apctl
    pspnet_resolver
    pspdebug
)

create_pbp_file(
    TARGET ${PROJECT_NAME}
    TITLE "PSP Store"
)
