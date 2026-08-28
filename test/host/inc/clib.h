#ifndef CLIB_H
#define CLIB_H
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define DP_IS_ARRAY(a) (!__builtin_types_compatible_p(__typeof__(a), __typeof__(&(a)[0])))
#define DP_BUILD_BUG_IF(e) ((int)(sizeof(struct { int _dpchk : (1 - 2 * !!(e)); })) * 0)
#define DP_SNPRINTF(arr, ...) snprintf((arr), sizeof(arr) + DP_BUILD_BUG_IF(!DP_IS_ARRAY(arr)), __VA_ARGS__)
#endif
