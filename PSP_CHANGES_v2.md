# BT3 PSP v2: getting past the memory card screen

Changed files: port/psp/psp_main.c, port/psp/psp_gs.c, port/src/plat_mc.c  (not compiled by me: no PSP toolchain here)

1. Frame skipping (the main fix). The software renderer needs seconds per picture, and the game's own clock only
   moves when its frames go by: the memory card check alone counts 120 frames before it looks at the card, so at
   1 picture/second it takes minutes, then more screens follow. Now a picture is left undrawn while the game is behind
   the 60 Hz vertical blank, at most 11 in a row. The game runs about 10x faster; the screen updates about once a second.
   Change the 11 with a skip.txt (a number, 0 = draw every frame) in ms0:/PSP/GAME/BT3PSP/ .
2. Save folder. The game looked only in ms0:/PSP/GAME/BT3PSP/saves. It now uses "saves" next to the EBOOT if there is
   one (so a folder called bt3 works), else the BT3PSP one. Layout it expects: saves/card1/BASLUS-21678DBZT3/ (icon.sys,
   dbzsm.ico, BASLUS-21678DBZT3). The log lists what it finds in the saves folder.
3. log.txt now has "memory card: request N -> result" for the first 60 card requests and the paths used, and the picture
   counters (drawn / left out) every 120 vblanks.

Send back log.txt (ms0:/PSP/GAME/BT3PSP/log.txt) after a run of 2 to 3 minutes.

## v3 (after the first PPSSPP run that reached the title screen)
- The picture repeated on the right: the game's frame is 512 wide (fbw 8), the copy assumed 640. Now it uses fbw * 64.
- Pixelated drawing: gs_core.c draws only every 2nd pixel in x and y (a quarter of the work). step.txt in the game's
  folder: 1 = full, 2 = default, 4 = very blocky and fastest. The screen copy reads the same pixels.
- Pad taps are latched: every pad sample since the last game frame is combined, so a short press is not lost when a
  game frame takes a second.
- The log line every second now shows pictures drawn / left out, and how long copying them took.
- No sound yet: the game's sound goes through the PS2's sound chip and CRI library, which are empty stand-ins here.
