#ifndef MONGOOSE_CUSTOM_H
#define MONGOOSE_CUSTOM_H

/* Standard headers for MG_ARCH_CUSTOM. The kernel is -nostdlib, but the
 * toolchain's freestanding headers (declarations) are available; the
 * implementations are provided as weak shims in mongoose_glue.c. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#endif /* MONGOOSE_CUSTOM_H */