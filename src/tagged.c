#include "tagged.h"

fr_tagged_kind_t fr_tagged_kind(fr_tagged_t tagged) {
  if (tagged <= FR_TAGGED_INT_END) {
    return FR_TAGGED_INT;
  } else if (tagged <= FR_TAGGED_SPECIAL_END) {
    return FR_TAGGED_SPECIAL;
  } else if (tagged <= FR_TAGGED_SLOT_END) {
    return FR_TAGGED_SLOT_ID;
  } else if (tagged <= FR_TAGGED_CODE_END) {
    return FR_TAGGED_CODE_OBJECT_ID;
  } else if (tagged <= FR_TAGGED_NATIVE_END) {
    return FR_TAGGED_NATIVE_ID;
  } else if (tagged <= FR_TAGGED_OBJECT_END) {
    return FR_TAGGED_OBJECT_ID;
#if FR_FEATURE_HANDLES
  } else if (tagged <= FR_TAGGED_HANDLE_END) {
    return FR_TAGGED_HANDLE;
#endif
  } else {
    return FR_TAGGED_RESERVED;
  }
}

bool fr_tagged_is_valid(fr_tagged_t tagged) {
  switch (fr_tagged_kind(tagged)) {
  case FR_TAGGED_INT:
  case FR_TAGGED_SPECIAL:
    return true;
  case FR_TAGGED_SLOT_ID:
    return (fr_tagged_t)(tagged - FR_TAGGED_SLOT_BASE) <=
           FR_TAGGED_SLOT_MAX_ID;
  case FR_TAGGED_CODE_OBJECT_ID:
    return (fr_tagged_t)(tagged - FR_TAGGED_CODE_BASE) <=
           FR_TAGGED_CODE_MAX_ID;
  case FR_TAGGED_NATIVE_ID:
    return (fr_tagged_t)(tagged - FR_TAGGED_NATIVE_BASE) <=
           FR_TAGGED_NATIVE_MAX_ID;
  case FR_TAGGED_OBJECT_ID:
    return (fr_tagged_t)(tagged - FR_TAGGED_OBJECT_BASE) <=
           FR_TAGGED_OBJECT_MAX_ID;
#if FR_FEATURE_HANDLES
  case FR_TAGGED_HANDLE:
    return true;
#endif
  case FR_TAGGED_RESERVED:
  default:
    return false;
  }
}

fr_tagged_t fr_tagged_nil(void) { return FR_TAGGED_NIL; }
fr_tagged_t fr_tagged_false(void) { return FR_TAGGED_FALSE; }
fr_tagged_t fr_tagged_true(void) { return FR_TAGGED_TRUE; }

bool fr_tagged_is_nil(fr_tagged_t tagged) { return tagged == FR_TAGGED_NIL; }
bool fr_tagged_is_false(fr_tagged_t tagged) {
  return tagged == FR_TAGGED_FALSE;
}
bool fr_tagged_is_true(fr_tagged_t tagged) { return tagged == FR_TAGGED_TRUE; }
bool fr_tagged_is_bool(fr_tagged_t tagged) {
  return tagged == FR_TAGGED_FALSE || tagged == FR_TAGGED_TRUE;
}

bool fr_tagged_is_falsy(fr_tagged_t tagged) {
  fr_int_t raw_int = 0;

  if (fr_tagged_is_nil(tagged) || fr_tagged_is_false(tagged)) {
    return true;
  }
  if (fr_tagged_decode_int(tagged, &raw_int) == FR_OK) {
    return raw_int == 0;
  }
  return false;
}

bool fr_tagged_can_encode_int(int32_t raw_int) {
  return raw_int >= FR_TAGGED_INT_MIN && raw_int <= FR_TAGGED_INT_MAX;
}

fr_err_t fr_tagged_encode_int(int32_t raw_int, fr_tagged_t *out_tagged) {
  if (!fr_tagged_can_encode_int(raw_int) || out_tagged == NULL) {
    return out_tagged == NULL ? FR_ERR_INVALID : FR_ERR_RANGE;
  }

  *out_tagged = (fr_tagged_t)(raw_int + FR_TAGGED_INT_BIAS);
  return FR_OK;
}

fr_err_t fr_tagged_decode_int(fr_tagged_t tagged, fr_int_t *out_int) {
  if (out_int == NULL) {
    return FR_ERR_INVALID;
  }
  if (tagged <= FR_TAGGED_INT_END) {
    *out_int = (fr_int_t)((int32_t)tagged - FR_TAGGED_INT_BIAS);
    return FR_OK;
  }
  return FR_ERR_TYPE;
}

fr_err_t fr_tagged_encode_bool(bool value, fr_tagged_t *out_tagged) {
  if (out_tagged == NULL) {
    return FR_ERR_INVALID;
  }
  *out_tagged = value ? FR_TAGGED_TRUE : FR_TAGGED_FALSE;
  return FR_OK;
}

fr_err_t fr_tagged_decode_bool(fr_tagged_t tagged, bool *out_value) {
  if (out_value == NULL) {
    return FR_ERR_INVALID;
  }
  if (tagged == FR_TAGGED_TRUE) {
    *out_value = true;
    return FR_OK;
  }
  if (tagged == FR_TAGGED_FALSE) {
    *out_value = false;
    return FR_OK;
  }
  return FR_ERR_TYPE;
}

/* The encode-side range check for compact IDs is #if FR_WORD_SIZE == 16 only;
 * the 32-bit branch trusts the typedef to fit the 32-bit band (UINT16_MAX).
 * Lock that here so widening fr_slot_id_t et al. past 16 bits without also
 * widening the band fails the build, not the runtime. */
typedef char fr_slot_id_fits_32bit_max_id
    [((fr_slot_id_t)-1 <= UINT16_MAX) ? 1 : -1];
typedef char fr_code_object_id_fits_32bit_max_id
    [((fr_code_object_id_t)-1 <= UINT16_MAX) ? 1 : -1];
typedef char fr_native_id_fits_32bit_max_id
    [((fr_native_id_t)-1 <= UINT16_MAX) ? 1 : -1];
typedef char fr_object_id_fits_32bit_max_id
    [((fr_object_id_t)-1 <= UINT16_MAX) ? 1 : -1];

fr_err_t fr_tagged_encode_slot_id(fr_slot_id_t slot_id,
                                  fr_tagged_t *out_tagged) {
  if (out_tagged == NULL) {
    return FR_ERR_INVALID;
  }
#if FR_WORD_SIZE == 16
  if ((fr_tagged_t)slot_id > FR_TAGGED_SLOT_MAX_ID) {
    return FR_ERR_RANGE;
  }
#endif

  *out_tagged = (fr_tagged_t)(slot_id + FR_TAGGED_SLOT_BASE);
  return FR_OK;
}

fr_err_t fr_tagged_decode_slot_id(fr_tagged_t tagged,
                                  fr_slot_id_t *out_slot_id) {
  if (out_slot_id == NULL) {
    return FR_ERR_INVALID;
  }
  if (tagged >= FR_TAGGED_SLOT_BASE && tagged <= FR_TAGGED_SLOT_END) {
    fr_tagged_t offset = (fr_tagged_t)(tagged - FR_TAGGED_SLOT_BASE);
    if (offset > FR_TAGGED_SLOT_MAX_ID) {
      return FR_ERR_RANGE;
    }
    *out_slot_id = (fr_slot_id_t)offset;
    return FR_OK;
  }
  return FR_ERR_TYPE;
}

fr_err_t fr_tagged_encode_code_object_id(fr_code_object_id_t code_object_id,
                                         fr_tagged_t *out_tagged) {
  if (out_tagged == NULL) {
    return FR_ERR_INVALID;
  }
#if FR_WORD_SIZE == 16
  if ((fr_tagged_t)code_object_id > FR_TAGGED_CODE_MAX_ID) {
    return FR_ERR_RANGE;
  }
#endif

  *out_tagged = (fr_tagged_t)(code_object_id + FR_TAGGED_CODE_BASE);
  return FR_OK;
}

fr_err_t
fr_tagged_decode_code_object_id(fr_tagged_t tagged,
                                fr_code_object_id_t *out_code_object_id) {
  if (out_code_object_id == NULL) {
    return FR_ERR_INVALID;
  }
  if (tagged >= FR_TAGGED_CODE_BASE && tagged <= FR_TAGGED_CODE_END) {
    fr_tagged_t offset = (fr_tagged_t)(tagged - FR_TAGGED_CODE_BASE);
    if (offset > FR_TAGGED_CODE_MAX_ID) {
      return FR_ERR_RANGE;
    }
    *out_code_object_id = (fr_code_object_id_t)offset;
    return FR_OK;
  }
  return FR_ERR_TYPE;
}

fr_err_t fr_tagged_encode_native_id(fr_native_id_t native_id,
                                    fr_tagged_t *out_tagged) {
  if (out_tagged == NULL) {
    return FR_ERR_INVALID;
  }
#if FR_WORD_SIZE == 16
  if ((fr_tagged_t)native_id > FR_TAGGED_NATIVE_MAX_ID) {
    return FR_ERR_RANGE;
  }
#endif

  *out_tagged = (fr_tagged_t)(native_id + FR_TAGGED_NATIVE_BASE);
  return FR_OK;
}

fr_err_t fr_tagged_decode_native_id(fr_tagged_t tagged,
                                    fr_native_id_t *out_native_id) {
  if (out_native_id == NULL) {
    return FR_ERR_INVALID;
  }
  if (tagged >= FR_TAGGED_NATIVE_BASE && tagged <= FR_TAGGED_NATIVE_END) {
    fr_tagged_t offset = (fr_tagged_t)(tagged - FR_TAGGED_NATIVE_BASE);
    if (offset > FR_TAGGED_NATIVE_MAX_ID) {
      return FR_ERR_RANGE;
    }
    *out_native_id = (fr_native_id_t)offset;
    return FR_OK;
  }
  return FR_ERR_TYPE;
}

fr_err_t fr_tagged_encode_object_id(fr_object_id_t object_id,
                                    fr_tagged_t *out_tagged) {
  if (out_tagged == NULL) {
    return FR_ERR_INVALID;
  }
#if FR_WORD_SIZE == 16
  if ((fr_tagged_t)object_id > FR_TAGGED_OBJECT_MAX_ID) {
    return FR_ERR_RANGE;
  }
#endif

  *out_tagged = (fr_tagged_t)(object_id + FR_TAGGED_OBJECT_BASE);
  return FR_OK;
}

fr_err_t fr_tagged_decode_object_id(fr_tagged_t tagged,
                                    fr_object_id_t *out_object_id) {
  if (out_object_id == NULL) {
    return FR_ERR_INVALID;
  }
  if (tagged >= FR_TAGGED_OBJECT_BASE && tagged <= FR_TAGGED_OBJECT_END) {
    fr_tagged_t offset = (fr_tagged_t)(tagged - FR_TAGGED_OBJECT_BASE);
    if (offset > FR_TAGGED_OBJECT_MAX_ID) {
      return FR_ERR_RANGE;
    }
    *out_object_id = (fr_object_id_t)offset;
    return FR_OK;
  }
  return FR_ERR_TYPE;
}

fr_err_t fr_tagged_encode_handle_ref(fr_handle_ref_t ref,
                                     fr_tagged_t *out_tagged) {
  if (out_tagged == NULL) {
    return FR_ERR_INVALID;
  }
#if FR_FEATURE_HANDLES
  if (ref.id > FR_TAGGED_HANDLE_MAX_ID ||
      ref.generation > FR_TAGGED_HANDLE_MAX_GENERATION) {
    return FR_ERR_RANGE;
  }

  *out_tagged =
      (fr_tagged_t)(FR_TAGGED_HANDLE_BASE | ref.id |
                    ((fr_tagged_t)ref.generation
                     << FR_TAGGED_HANDLE_GENERATION_SHIFT));
  return FR_OK;
#else
  (void)ref;
  return FR_ERR_UNSUPPORTED;
#endif
}

uint32_t fr_read_u32_le(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

void fr_write_u32_le(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)(value & 0xffu);
  bytes[1] = (uint8_t)((value >> 8) & 0xffu);
  bytes[2] = (uint8_t)((value >> 16) & 0xffu);
  bytes[3] = (uint8_t)((value >> 24) & 0xffu);
}

fr_err_t fr_tagged_decode_handle_ref(fr_tagged_t tagged,
                                     fr_handle_ref_t *out_ref) {
  if (out_ref == NULL) {
    return FR_ERR_INVALID;
  }
#if FR_FEATURE_HANDLES
  if (tagged >= FR_TAGGED_HANDLE_BASE && tagged <= FR_TAGGED_HANDLE_END) {
    fr_tagged_t offset = (fr_tagged_t)(tagged - FR_TAGGED_HANDLE_BASE);

    out_ref->id = (fr_handle_id_t)(offset & FR_TAGGED_HANDLE_MAX_ID);
    out_ref->generation =
        (fr_handle_generation_t)((offset >> FR_TAGGED_HANDLE_GENERATION_SHIFT) &
                                 FR_TAGGED_HANDLE_MAX_GENERATION);
    return FR_OK;
  }
#else
  (void)tagged;
#endif
  return FR_ERR_TYPE;
}
