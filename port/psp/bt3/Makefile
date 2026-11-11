# BT3 PSP - Stage 0a probe. Build with the pspdev toolchain:  make
TARGET = BT3PROBE
OBJS   = main.o

CFLAGS   = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS  = $(CFLAGS)

# ask ARK for the big user partition (MEMSIZE=1 in PARAM.SFO); run "make clean" after changing it
PSP_LARGE_MEMORY = 1

LIBS = -lpspdebug -lpspdisplay -lpspge -lpspctrl -lpsppower -lpsprtc -lm

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = BT3 PSP Probe

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
