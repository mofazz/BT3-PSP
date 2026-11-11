/* PSP stage: the GS front end runs on the game's thread (software renderer), so its locks have nothing to wait for.
   Every name is renamed, so that this header cannot clash with a C library's own pthread types. */
#ifndef PSP_PTHREAD_SHIM_H
#define PSP_PTHREAD_SHIM_H
/* the C library's own headers first: whatever they declare about threads is done before the names below are renamed */
#include <sys/types.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#define pthread_t psp_pthread_t
#define pthread_attr_t psp_pthread_attr_t
#define pthread_mutex_t psp_pthread_mutex_t
#define pthread_cond_t psp_pthread_cond_t
#define pthread_mutex_lock psp_pthread_mutex_lock
#define pthread_mutex_unlock psp_pthread_mutex_unlock
#define pthread_cond_wait psp_pthread_cond_wait
#define pthread_cond_broadcast psp_pthread_cond_broadcast
#define pthread_cond_signal psp_pthread_cond_signal
#define pthread_create psp_pthread_create
#define pthread_join psp_pthread_join
#undef PTHREAD_MUTEX_INITIALIZER
#undef PTHREAD_COND_INITIALIZER
#define PTHREAD_MUTEX_INITIALIZER 0
#define PTHREAD_COND_INITIALIZER 0
typedef int psp_pthread_t;
typedef int psp_pthread_attr_t;
typedef int psp_pthread_mutex_t;
typedef int psp_pthread_cond_t;
static inline int psp_pthread_mutex_lock(psp_pthread_mutex_t *m) { (void)m; return 0; }
static inline int psp_pthread_mutex_unlock(psp_pthread_mutex_t *m) { (void)m; return 0; }
static inline int psp_pthread_cond_wait(psp_pthread_cond_t *c, psp_pthread_mutex_t *m) { (void)c; (void)m; return 0; }
static inline int psp_pthread_cond_broadcast(psp_pthread_cond_t *c) { (void)c; return 0; }
static inline int psp_pthread_cond_signal(psp_pthread_cond_t *c) { (void)c; return 0; }
static inline int psp_pthread_create(psp_pthread_t *t, const psp_pthread_attr_t *a, void *(*f)(void *), void *arg) { (void)t; (void)a; (void)f; (void)arg; return -1; }
static inline int psp_pthread_join(psp_pthread_t t, void **r) { (void)t; (void)r; return 0; }
#endif
