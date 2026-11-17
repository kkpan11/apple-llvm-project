#pragma clang system_header
#include <ptrcheck.h>
static inline int *sdk_forge(int *p) {
  return __unsafe_forge_single(int *, p); // sdk-warning{{use of '__unsafe_forge_single' is unsafe}}
}
