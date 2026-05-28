#pragma once

#include "types.h"

#if FR_FEATURE_INTROSPECTION
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
