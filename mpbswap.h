#ifndef MPBSWAP_H
#define MPBSWAP_H

#include "libavutil/bswap.h"

#ifdef __DJGPP__
#ifndef HAVE_SWAB
#define HAVE_SWAB 1
#endif
#endif

#ifndef HAVE_SWAB
void swab(const void *from, void *to, ssize_t n);
#endif

#endif /* MPBSWAP_H */
