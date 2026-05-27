#include "base_defs.h"

#include "board.h"

const fr_base_def_t fr_board_base_defs[] = {
    {
        .slot_id = FR_SLOT_LED_BUILTIN,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$led_builtin",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TAGGED_INT_LITERAL(FR_BOARD_LED_BUILTIN),
    },
};

const uint16_t fr_board_base_def_count =
    (uint16_t)(sizeof(fr_board_base_defs) / sizeof(fr_board_base_defs[0]));
