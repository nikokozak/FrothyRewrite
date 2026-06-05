#include "types.h"

#include <stddef.h>

const char *fr_err_name(fr_err_t err) {
  switch (err) {
  case FR_ERR_RANGE:
    return "out of range";
  case FR_ERR_TYPE:
    return "wrong type";
  case FR_ERR_DOMAIN:
    return "bad value";
  case FR_ERR_CAPACITY:
    return "capacity exceeded";
  case FR_ERR_OVERFLOW:
    return "overflow";
  case FR_ERR_UNDERFLOW:
    return "underflow";
  case FR_ERR_NOT_FOUND:
    return "not found";
  case FR_ERR_INVALID:
    return "bad source";
  case FR_ERR_UNSUPPORTED:
    return "unsupported";
  case FR_ERR_INTERRUPTED:
    return "interrupted";
  case FR_ERR_CORRUPT:
    return "corrupt data";
  case FR_ERR_IO:
    return "i/o failed";
  case FR_ERR_VOLATILE:
    return "not saved";
  case FR_ERR_HANDLE:
    return "bad handle";
  case FR_ERR_NET_DISCONNECTED:
    return "no network";
  case FR_ERR_NET_TIMEOUT:
    return "timed out";
  case FR_ERR_NET_DNS:
    return "dns failed";
  case FR_ERR_NET_REFUSED:
    return "refused";
  case FR_ERR_NET_TOO_LARGE:
    return "too large";
  case FR_ERR_NET_PROTOCOL:
    return "bad protocol";
  case FR_OK:
  default:
    return NULL;
  }
}
