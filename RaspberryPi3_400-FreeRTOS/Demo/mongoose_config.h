#ifndef MONGOOSE_CONFIG_H
#define MONGOOSE_CONFIG_H

/* Standard headers for the custom (bare-metal, -nostdlib) build. These are
 * included early by mongoose.h, so the integer types etc. are available
 * throughout mongoose.c. The freestanding ones (stdint/stddef/stdarg/stdbool)
 * are always present; string/stdlib/ctype give declarations only -- the
 * implementations are weak shims in mongoose_glue.c. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

/* Custom architecture + built-in TCP/IP stack, custom time base + RNG,
 * no filesystem, manual driver init. */
#define MG_ARCH                     MG_ARCH_CUSTOM
#define MG_ENABLE_TCPIP             1
#define MG_ENABLE_CUSTOM_MILLIS     1
#define MG_ENABLE_CUSTOM_RANDOM     1
#define MG_ENABLE_PACKED_FS         0
#define MG_ENABLE_FILE              0
#define MG_ENABLE_DIRLIST           0
#define MG_ENABLE_TCPIP_DRIVER_INIT 0
#define MG_ENABLE_LOG               1

#endif /* MONGOOSE_CONFIG_H */