set(FR_KERNEL_SOURCES
    ${FR_REWRITE_ROOT}/src/tagged.c
    ${FR_REWRITE_ROOT}/src/crc.c
    ${FR_REWRITE_ROOT}/src/slot.c
    ${FR_REWRITE_ROOT}/src/profile.c
    ${FR_REWRITE_ROOT}/src/runtime.c
    ${FR_REWRITE_ROOT}/src/instruction.c
    ${FR_REWRITE_ROOT}/src/code.c
    ${FR_REWRITE_ROOT}/src/native.c
    ${FR_REWRITE_ROOT}/src/handle.c
    ${FR_REWRITE_ROOT}/src/object.c
    ${FR_REWRITE_ROOT}/src/image.c
    ${FR_REWRITE_ROOT}/src/base_defs.c
    ${FR_REWRITE_ROOT}/src/base_image.c
    ${FR_REWRITE_ROOT}/src/vm.c
    ${FR_REWRITE_ROOT}/src/parse.c
    ${FR_REWRITE_ROOT}/src/compile.c
    ${FR_REWRITE_ROOT}/src/repl.c
    ${FR_REWRITE_ROOT}/src/persist.c
    ${FR_REWRITE_ROOT}/src/persist_payload.c)

idf_component_register(
    SRCS
        main.c
        ${FR_KERNEL_SOURCES}
        ${FR_REWRITE_ROOT}/targets/common/target_defs.c
        ${FR_REWRITE_ROOT}/targets/esp-idf/platform.c
        ${FR_REWRITE_ROOT}/boards/${FR_REWRITE_BOARD}/board_defs.c
    INCLUDE_DIRS
        ${FR_REWRITE_ROOT}/src
        ${FR_REWRITE_ROOT}/profiles
        ${FR_REWRITE_ROOT}/targets/esp-idf
        ${FR_REWRITE_ROOT}/boards/${FR_REWRITE_BOARD}
    REQUIRES
        driver
        esp_adc
        esp_timer
        nvs_flash)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FR_PROFILE_HEADER="${FR_REWRITE_PROFILE}.h"
    FR_PROFILE_NAME="${FR_REWRITE_PROFILE}")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu17
    -Wall
    -Wextra
    -Werror
    -Wno-error=missing-field-initializers)
