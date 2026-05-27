#include "base_defs.h"

#include "board.h"

enum {
  FR_SLOT_A0 = FR_SLOT_BOARD_LOCAL_BASE,
  FR_SLOT_BOOT_BUTTON = FR_SLOT_BOARD_LOCAL_BASE + 1,
};

const fr_base_def_t fr_board_base_defs[] = {
    {
        .slot_id = FR_SLOT_LED_BUILTIN,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$led_builtin",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TAGGED_INT_LITERAL(FR_BOARD_LED_BUILTIN),
    },
    {
        .slot_id = FR_SLOT_A0,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$a0",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TAGGED_INT_LITERAL(FR_BOARD_A0),
    },
    {
        .slot_id = FR_SLOT_BOOT_BUTTON,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$boot_button",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TAGGED_INT_LITERAL(FR_BOARD_BOOT_BUTTON),
    },
};

const uint16_t fr_board_base_def_count =
    (uint16_t)(sizeof(fr_board_base_defs) / sizeof(fr_board_base_defs[0]));
