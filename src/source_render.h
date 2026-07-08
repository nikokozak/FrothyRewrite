#pragma once

#include "instruction.h"
#include "types.h"

#if FR_FEATURE_INTROSPECTION
/* Shared `see` scratch view. If a code object has direct bytes, this returns
 * that view. If not, it reads one definition through the code reader into the
 * renderer's single-shot scratch. */
fr_err_t fr_source_render_instruction_view(fr_runtime_t *runtime,
                                           fr_code_object_id_t code_object_id,
                                           fr_instruction_stream_t *out_view);

/* Rebuild `to <name> [ body ]` source from a code object's bytecode and stream
 * it through write(ctx, text). Returns FR_ERR_UNSUPPORTED when the body uses a
 * shape this renderer can't reconstruct yet; the caller then prints the
 * bytecode listing instead. see must never error, so this is never the only
 * path. */
fr_err_t fr_source_render_code(fr_runtime_t *runtime,
                               fr_code_object_id_t code_object_id,
                               const char *word_name,
                               fr_err_t (*write)(void *ctx, const char *text),
                               void *ctx);
#endif
