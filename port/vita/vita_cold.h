/* What only the log and the measurements call: out of the way of the code around them (the compiler
   takes the branches that reach these as rarely taken and puts them apart: -freorder-blocks-and-partition), so that
   the drawing thread's busy routines are not spread over the instruction cache with the log's lines. */
#ifndef VITA_COLD_H
#define VITA_COLD_H
extern void Vita_Log(const char *fmt, ...) __attribute__((cold));
extern void Vita_Z(int z) __attribute__((cold));
#ifndef VITA_RARE
#define VITA_RARE(x) __builtin_expect_with_probability(!!(x), 1, 0.0)
#endif
#endif
