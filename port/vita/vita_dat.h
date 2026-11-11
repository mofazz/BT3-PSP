/* One run of bytes of the game's data tables: where it lives in this program and where its values are on the disc
   (source 0: SLUS_216.78 as a flat image from 0x100000, source 1: BIN/DBZP.BIN). Table: build.py -> vita_dat.c. */
#ifndef VITA_DAT_H
#define VITA_DAT_H
typedef struct PortDatRun {
    unsigned char *dst;
    unsigned source, offset, length;
} PortDatRun;
extern const PortDatRun gPortDatRuns[];
extern const unsigned gPortDatCount;
extern const unsigned long long gPortDatChecksum;
#endif
