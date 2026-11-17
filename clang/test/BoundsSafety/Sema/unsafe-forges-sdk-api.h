#pragma clang system_header
#include <ptrcheck.h>
#include <stddef.h>

// SDK declarations whose pointers are __unsafe_indexable by default.
int * /*__unsafe_indexable*/ getInt(void);
const char * /*__unsafe_indexable*/ get_version(void);
void * /*__unsafe_indexable*/ alloc(size_t size);
