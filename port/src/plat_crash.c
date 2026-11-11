/*
 * Crash report for the PC build: on a fatal signal, writes the signal and a backtrace (function names; the build
 * is linked without PIE and with its symbols) to the terminal and to bt3_crash.txt in the current directory, then
 * lets the default action happen. So a crash someone else hits can be diagnosed from one file.
 */
#ifdef _WIN32
/* Windows: the exception code, the faulting address and the return addresses on the stack. The program is linked
   at a fixed address (0x20000000), so the numbers can be looked up in bt3.exe.map of the same build. */
#include <windows.h>
#include <stdio.h>

static LONG WINAPI on_crash(EXCEPTION_POINTERS *e) {
    void *frames[48];
    USHORT n = RtlCaptureStackBackTrace(0, 48, frames, NULL), i;
    int k;

    for (k = 0; k < 2; k++) {
        FILE *out = k == 0 ? stderr : fopen("bt3_crash.txt", "w");
        if (out == NULL) {
            continue;
        }
        fprintf(out, "bt3: crashed: exception %08lX at %p", (unsigned long)e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
        if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2) {
            fprintf(out, " (%s address %p)", e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                    (void *)e->ExceptionRecord->ExceptionInformation[1]);
        }
        fprintf(out, "\nbacktrace (innermost first; addresses of bt3.exe, see bt3.exe.map):\n");
        for (i = 0; i < n; i++) {
            fprintf(out, "%p\n", frames[i]);
        }
        if (k == 1) {
            fclose(out);
            fprintf(stderr, "bt3: the same report is in bt3_crash.txt\n");
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

__attribute__((constructor)) static void crash_init(void) {
    /* The program is a window program (no console window of its own). Started from a terminal, its messages go
       to that terminal; started by the setup with a pipe, the pipe is kept. */
    if (GetStdHandle(STD_OUTPUT_HANDLE) == NULL && AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    SetUnhandledExceptionFilter(on_crash);
}
#else
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static void put(int fd, const char *s) {
    if (write(fd, s, strlen(s)) < 0) {
    }
}

static void on_crash(int sig) {
    void *frames[48];
    int n = backtrace(frames, 48), fd = open("bt3_crash.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644), k;
    const char *name = sig == SIGSEGV ? "SIGSEGV (bad memory access)" : sig == SIGFPE ? "SIGFPE (arithmetic)" :
                       sig == SIGILL ? "SIGILL (bad instruction)" : sig == SIGBUS ? "SIGBUS" : "SIGABRT";

    for (k = 0; k < 2; k++) {
        int out = k == 0 ? 2 : fd;
        if (out < 0) {
            continue;
        }
        put(out, "bt3: crashed: ");
        put(out, name);
        put(out, "\nbacktrace (innermost first):\n");
        backtrace_symbols_fd(frames, n, out);
    }
    if (fd >= 0) {
        close(fd);
        put(2, "bt3: the same report is in bt3_crash.txt\n");
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

__attribute__((constructor)) static void crash_init(void) {
    static const int sigs[] = {SIGSEGV, SIGFPE, SIGILL, SIGBUS, SIGABRT};
    unsigned i;

    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        signal(sigs[i], on_crash);
    }
}
#endif
