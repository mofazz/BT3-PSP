# port/psp - PSP-3000 port (work in progress)

Staged plan: 0a probe, **0b game logic with timing**, 1 menus, 2 fights, 3 speed.

## Stage 0a: probe/
Measures memory, real CPU clock, loop speed and disc files on the console (EBOOT titled "BT3 PSP").

    cd port/psp/probe && ./build.sh

## Stage 0b: the game's logic on the PSP, no picture, timed
Builds the decompiled game for the PSP with the Vita port's memory model (PORT_VITA = 32-bit target, hardware registers
in ordinary memory, data tables read from your disc at start). Nothing is drawn: `psp_glue.c` answers every call of the
Vita's renderer with "idle". The attract-demo fight runs (`BT3_DEMO`, `headless.c`) and `Vita_VBlank` (psp_main.c)
logs how long each 120 frames took.

Files: `build.py` (compile + link + EBOOT), `psp_main.c` (start-up, log, frame timer), `psp_mem.c` (game heap from the
user partition, data tables), `psp_file.c` (AFS archives), `psp_float.c` (float helpers, MIPS FCSR), `psp_glue.c`
(renderer / pad / sound stand-ins), `psp_shim.h`.

### Build
    pip install libclang            # the Vita build's source rewriting needs it (or BT3_LIBCLANG=/path/libclang.so)
    export PSPDEV=$HOME/pspdev; export PATH=$PATH:$PSPDEV/bin
    PSP_MS_ROOT=/path/to/memory/stick python3 port/psp/build.py

Messages to expect while it is new: `FAILED <file> <first error>` (full text in `port/build/obj_psp/errors.txt`),
`link: N symbols without a definition, stubbed` (the list is in `port/build/psp/undefined*.txt`; a stubbed function that
is called stops the program and the log names it), `link: FAILED` (full text in `port/build/psp/make-output.txt`).

### Run
Disc files in `ms0:/PSP/GAME/bt3/disc/` (SLUS_216.78, BIN/, DATA/). Start "BT3 PSP" from the XMB (ARK, extra memory on).
The log is `ms0:/PSP/GAME/BT3PSP/log.txt`. Every line of the start-up is written and closed at once (the card records a file size only on close), later lines go through a buffer that a heartbeat thread writes once a second together with an "alive:" line (vblank count, last stand-in function called). If the game freezes, the last "alive" line says whether it was running or stuck.

### Modes (stage 0b+)
- **Default: menus.** The game starts normally and the project's software GS renderer (`gs_core.c`, `gs_vu1.c`) draws into emulated
  GS memory; `psp_gs.c` copies each finished frame to the screen (640x448 -> 480x272, nearest). Pad: D-pad, face buttons,
  L/R = L1/R1, SELECT+L/R = L2/R2, START. It is slow (the rasteriser works in `double`, which the PSP does in software):
  expect seconds per frame at first. 60 Hz pacing via `sceDisplayWaitVblankStart`.
- **Timing run:** hold TRIANGLE while the game starts, or put an empty `demo.txt` in `ms0:/PSP/GAME/BT3PSP/`. The attract-demo
  fight runs without a picture and the log gives the microseconds per vblank (see above).

### Fixes from the first run's log
`Battle_Load` waits for `Snd_IsUploadDone()`, which is `func_002967C0(1, 0x80F0, ...)` answering 1. The first build's
automatic stub answered 0, so the demo hung at 24 us/vblank. `psp_glue.c` now answers it, `sceSifCallRpc` (reply word),
`sceSifSetDma` (copy) and `scePerfArmPmonGetCounterValue` (writes its output) as `port/src/gs/snd_se.c` does on the PC. Every
stand-in in `psp_glue.c` is weak, so a definition in the game or in the GS front end always wins.

### Crash hunting
With BT3_PSP_TRACE=1 `build.py` compiles the game with `-finstrument-functions` (off by default: it slows the game several times). The heartbeat thread saves the 64
functions entered last to `ms0:/PSP/GAME/BT3PSP/crumbs.txt` about every 50 ms; after a crash run
`python3 port/psp/tools/addr2func.py BT3PSP.elf crumbs.txt` (the ELF is `port/build/psp/BT3PSP.elf`). The log's `alive:` line
also shows the game thread's free stack (`sceKernelGetThreadStackFreeSize`).

### Power and clock
`log.txt` starts with the battery (percent, mV, temperature), the clock the firmware reports, and repeats it in every `alive:` line.
`clock.txt` (a number, MHz) in `ms0:/PSP/GAME/BT3PSP/` sets the CPU clock (default 333). Run with the ARK overclock off while testing.

### Where the disc files go
Anywhere of these (checked in this order; the log lists what was tried): `<folder of the EBOOT>/disc/`, the EBOOT's own folder, `<folder of the EBOOT>/DISC/`,
`ms0:/PSP/GAME/bt3/disc/` beside it, then `ms0:/PSP/GAME/bt3/disc`, `ms0:/bt3/disc`, `ms0:/GAME/bt3/disc`. Each holds `SLUS_216.78`, `BIN/`, `DATA/`.
In PPSSPP the memory stick is its own `memstick` folder (Settings > Tools or File > Open Memory Stick): put the files under `PSP/GAME/BT3PSP/disc/` there.

### Zero divides
The game's PS2 code divides by zero now and then (the PS2's divide does not trap). `-mno-check-zero-division` is set for every PSP compile: the MIPS32
compiler's default inserts a trap there, which a console cannot survive.

### Crash catcher (needs ARK / a custom firmware)
`exc/` is the crash catcher of PS2RC (`psp_exc`: a kernel module `ps2rc_exc.prx` plus its user side), used under the PS2RC License (see `exc/LICENSE`,
`exc/NOTICE.md`, unchanged). `python3 port/psp/build.py` also builds `ps2rc_exc.prx` (PSP_MS_ROOT copies it); it must sit **next to the EBOOT**
(or in `ms0:/PSP/GAME/BT3PSP/`). When the game faults on a real PSP the console used to freeze and switch off; now the registers go to
`crash.txt` and `log.txt` (EPC = where it died, BadVAddr = the address it touched, the exception kind) and the screen waits for CROSS. Look EPC up with
`tools/addr2func.py BT3PSP.elf <file with the address>`.

Based on PS2RC by SandstoneDev
https://github.com/SandstoneDev/PS2RC
Community: https://discord.gg/wV8x46ZTDu

### GPU mode (gu.txt) and the debug overlay
`psp_gu.c` is a back end for the GS front end that draws through the PSP's GE (sceGu) instead of the software rasteriser: put an empty
`gu.txt` in `ms0:/PSP/GAME/BT3PSP/` (delete it to go back). Only the frame's own buffer is drawn (the glow/blur/shadow work buffers are
left out), textures are read out of the emulated GS memory into a small cache (8-bit + palette where the game does that), 640 x 448
becomes 480 x 272. The VU1 programs still run on the CPU (gs_vu1.c interprets them; the triangles arrive in `GsGpu_Draw`).
SELECT + START toggles the overlay (mode, pictures per second, GS ms per picture, texture counts, free RAM, stack, clocks, battery).
In a battle the D-pad's up and down are R1 and R2. The exit callback logs each step of leaving (`log.txt`: "exit: ...").
