# BT3 PSP - Stage 0a probe

A tiny EBOOT that measures, on your PSP-3000, whether the BT3 port is feasible before any game code is built.

## Build
    cd bt3psp_probe
    make clean && make          # needs the pspdev toolchain (psp-gcc, PSPSDK) on PATH
Copy the folder to `ms0:/PSP/GAME/BT3PROBE/` (the PBP is `EBOOT.PBP`).

## Run
Start it from the XMB with ARK's extra memory ON. Run it twice: once with the CPU clock at 333, once at your 444 setting.
Send me `ms0:/PSP/GAME/BT3PROBE/probe.txt` (or a photo of the screen) from both runs.

## Disc files
Put your own USA disc files at `ms0:/bt3/disc/` (the probe also tries ms0:/GAME/bt3/disc, ms0:/PSP/GAME/bt3/disc and a few more) : `SLUS_216.78`, `BIN/`, `DATA/` (same layout as the Vita port).

## What the numbers mean
- largest block >= 40 MB: the 64 MB mode is on. The port needs about 28.3 MB heap plus ~4-13 MB of code.
- "about N MHz" from the int loop is the real clock. If it says ~333 under your 444 setting, the overclock is not applying.
- float / div / memcpy numbers are yardsticks for how heavy the game's logic can be per 33 ms frame.
