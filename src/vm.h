#pragma once

#include "instruction.h"
#include "runtime.h"

fr_err_t fr_vm_run_instruction_stream(fr_runtime_t *runtime,
                                      const fr_instruction_stream_t *view,
                                      fr_tagged_t *out_tagged);
fr_err_t fr_vm_run_code_object(fr_runtime_t *runtime,
                               fr_code_object_id_t code_object_id,
                               fr_tagged_t *out_tagged);
fr_err_t fr_vm_run_slot(fr_runtime_t *runtime, fr_slot_id_t slot_id,
                        fr_tagged_t *out_tagged);
fr_err_t fr_vm_run_boot(fr_runtime_t *runtime, fr_tagged_t *out_tagged);
