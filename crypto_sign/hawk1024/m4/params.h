/*
 * params.h — HAWK-1024 parameters (Round-2 spec, May 2025).
 */

#ifndef HAWK1024_M4_PARAMS_H
#define HAWK1024_M4_PARAMS_H

#define HAWK_LOGN    10
#define HAWK_N      (1 << HAWK_LOGN)   /* 1024 */

#define HAWK_Q       18433
#define HAWK_QINV    /* TODO */
#define HAWK_R2MOD   /* TODO */

#endif /* HAWK1024_M4_PARAMS_H */
