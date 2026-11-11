/* Maps the few PS Vita system calls the shared port files use onto the PSP SDK. */
#ifndef PSP_SHIM_H
#define PSP_SHIM_H
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psptypes.h>
#define SCE_O_RDONLY PSP_O_RDONLY
#define SCE_SEEK_SET PSP_SEEK_SET
#define SCE_SEEK_END PSP_SEEK_END
/* the PSP has no positional read: seek, then read (the port reads from one thread in this stage) */
static inline int sceIoPread(SceUID fd, void *buf, SceSize n, SceOff off) {
    if (sceIoLseek(fd, off, PSP_SEEK_SET) < 0) {
        return -1;
    }
    return sceIoRead(fd, buf, n);
}
#define sceKernelGetProcessTimeWide() ((unsigned long long)sceKernelGetSystemTimeWide())
#endif
