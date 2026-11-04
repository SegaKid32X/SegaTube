# ======================================================================
# SegaTube - a movie player for the Sega 32X (+ Mega EverDrive PRO)
#
#   make            rom/segatube.32x            the player (movies from the SD card)
#   make test-rom   rom/segatube-test.32x       with a generated test movie built in
#   make tools      obj/stvenc, obj/stvplay     the encoder and the desktop player
#   make check      static ROM checks + host tests (+ PicoDrive if tests/emu has it)
#   make clean
#
# Toolchain: sh-elf and m68k-elf GCC in $(GENDEV) (Chilly Willy's 32XDK layout;
# see docs/DESIGN.md section 9 for building one from Ubuntu's GCC sources).
# ======================================================================

GENDEV ?= /opt/toolchains/sega
TARGET  := segatube
OBJ     := obj
ROM     := rom/$(TARGET).32x
TESTROM := rom/$(TARGET)-test.32x

SHPREFIX := $(GENDEV)/sh-elf/bin/sh-elf-
MDPREFIX := $(GENDEV)/m68k-elf/bin/m68k-elf-
CC     := $(SHPREFIX)gcc
AS     := $(SHPREFIX)as
OBJC   := $(SHPREFIX)objcopy
SIZE   := $(SHPREFIX)size
MDAS   := $(MDPREFIX)as
MDCC   := $(MDPREFIX)gcc
MDOBJC := $(MDPREFIX)objcopy
HOSTCC ?= cc

MDCFLAGS := -m68000 -O2 -fomit-frame-pointer -ffreestanding -nostdlib -Wall -Wextra \
            -Wno-unused-parameter -I$(OBJ) -Isrc/platform/include
# One optimisation level for all SH-2 code (GCC on the SH-2 mixes -O levels badly).
SHFLAGS := -c -std=gnu11 -m2 -mb -O2 -fomit-frame-pointer -ffreestanding \
           -ffunction-sections -fdata-sections -fno-common \
           -Wall -Wextra -Wno-unused-parameter -D__32X__ \
           -Isrc/codec -Isrc/platform/32x -Isrc/platform/include -I$(OBJ) $(EXTRA_CFLAGS)
LDFLAGS := -T src/platform/32x/mars.ld -nostdlib -Wl,--gc-sections -Wl,-Map=$(OBJ)/$(TARGET).map
LIBS    := -lgcc

SH_SRC := $(wildcard src/platform/32x/*.c) src/codec/stv_dec.c
SH_OBJS := $(OBJ)/crt0.o $(patsubst %.c,$(OBJ)/sh/%.o,$(notdir $(SH_SRC)))
MD_OBJS := $(OBJ)/md_crt.o $(OBJ)/md_halt.o $(OBJ)/osd_tiles.o \
           $(patsubst src/platform/md/%.c,$(OBJ)/md/%.o,$(wildcard src/platform/md/*.c))

MARKERS := --title "SEGATUBE" --marker "SEGATUBE PLAYER" --marker "STVFS@"

.PHONY: all tools test-rom test-rom-long probe check clean host-test
all: $(ROM)

$(OBJ) $(OBJ)/sh $(OBJ)/md rom:
	mkdir -p $@

# ---- host tools ----
tools: $(OBJ)/stvenc $(OBJ)/stvplay
$(OBJ)/stvenc: tools/stvenc/stvenc.c src/codec/stv_dec.c src/codec/*.h | $(OBJ)
	$(HOSTCC) -O2 -std=gnu11 -Wall -Wextra -Isrc/codec -o $@ tools/stvenc/stvenc.c src/codec/stv_dec.c -lm
$(OBJ)/stvplay: tests/host/stvplay.c src/codec/stv_dec.c src/codec/*.h | $(OBJ)
	$(HOSTCC) -O2 -std=gnu11 -Wall -Wextra -Isrc/codec -o $@ tests/host/stvplay.c src/codec/stv_dec.c

# ---- 68000 program ----
$(OBJ)/osd_tiles.s $(OBJ)/osd_tiles.h: tools/gen_font.py tools/font5x7.py | $(OBJ)
	python3 tools/gen_font.py $(OBJ)/osd_tiles.s $(OBJ)/osd_tiles.h
$(OBJ)/osd_tiles.o: $(OBJ)/osd_tiles.s
	$(MDAS) -m68000 $< -o $@
$(OBJ)/md_crt.o: src/platform/md/md_crt.s | $(OBJ)
	$(MDAS) -m68000 --register-prefix-optional $< -o $@
$(OBJ)/md_halt.o: src/platform/md/md_halt.s | $(OBJ)
	$(MDAS) -m68000 --register-prefix-optional $< -o $@
$(OBJ)/md/%.o: src/platform/md/%.c src/platform/md/md.h $(OBJ)/osd_tiles.h | $(OBJ)/md
	$(MDCC) $(MDCFLAGS) -c $< -o $@
$(OBJ)/md.bin: $(MD_OBJS) src/platform/md/md.ld
	$(MDCC) -m68000 -nostdlib -T src/platform/md/md.ld $(MD_OBJS) -lgcc -o $(OBJ)/md.elf -Wl,-Map=$(OBJ)/md.map
	$(MDOBJC) -O binary $(OBJ)/md.elf $@
	@echo "68000 program: $$(stat -c %s $@) bytes"

# ---- SH-2 program ----
$(OBJ)/crt0.o: src/platform/32x/crt0.s $(OBJ)/md.bin src/platform/32x/icd_mars.bin
	$(AS) --big -I$(OBJ) $< -o $@
$(OBJ)/sh/%.o: src/platform/32x/%.c src/platform/32x/*.h src/codec/*.h | $(OBJ)/sh
	$(CC) $(SHFLAGS) $< -o $@
$(OBJ)/sh/stv_dec.o: src/codec/stv_dec.c src/codec/*.h | $(OBJ)/sh
	$(CC) $(SHFLAGS) $< -o $@
$(OBJ)/$(TARGET).elf: $(SH_OBJS) src/platform/32x/mars.ld
	$(CC) $(LDFLAGS) $(SH_OBJS) $(LIBS) -o $@
	$(SIZE) $@
	@grep -E '^ +0x[0-9a-f]+ +__bss_end' $(OBJ)/$(TARGET).map || true
$(OBJ)/$(TARGET).raw: $(OBJ)/$(TARGET).elf
	$(OBJC) -O binary $< $@

$(ROM): $(OBJ)/$(TARGET).raw | rom
	cp $< $@
	python3 tools/romfix.py $@ --pow2 --gran 524288
	python3 tests/verify_rom.py $@ $(OBJ)/$(TARGET).elf --title SEGATUBE --marker "SEGATUBE PLAYER" --min-text 2048

# ---- the test ROM: a generated clip, every mode, built in ----
TESTCLIP := $(OBJ)/testclip/clip.mp4
TESTMOVIE := $(OBJ)/testsd/MOVIES/SegaTube-Test
$(TESTCLIP): tools/make_test_clip.sh
	sh tools/make_test_clip.sh $@
$(TESTMOVIE)/INFO.STI: $(TESTCLIP) $(OBJ)/stvenc converter/segatube_convert.py
	rm -rf $(OBJ)/testsd
	python3 converter/segatube_convert.py $(TESTCLIP) --out $(OBJ)/testsd --title "SegaTube-Test" \
	    --quality 4 --duration 3
test-rom: $(OBJ)/$(TARGET).raw $(TESTMOVIE)/INFO.STI | rom
	python3 tools/mkvfs.py $(OBJ)/$(TARGET).raw $(TESTROM) "$(TESTMOVIE)"
	python3 tools/romfix.py $(TESTROM) --pow2 --gran 524288
	python3 tests/verify_rom.py $(TESTROM) $(OBJ)/$(TARGET).elf $(MARKERS) --min-text 2048

# a longer movie, one mode, for the control tests (scan, scrub, the end)
LONGMOVIE := $(OBJ)/testsd-long/MOVIES/Long-Test
$(LONGMOVIE)/INFO.STI: $(TESTCLIP) $(OBJ)/stvenc converter/segatube_convert.py
	rm -rf $(OBJ)/testsd-long
	python3 converter/segatube_convert.py $(TESTCLIP) --out $(OBJ)/testsd-long --title "Long-Test" \
	    --modes full --quality 3 --duration 11
test-rom-long: $(OBJ)/$(TARGET).raw $(LONGMOVIE)/INFO.STI | rom
	python3 tools/mkvfs.py $(OBJ)/$(TARGET).raw rom/$(TARGET)-long.32x "$(LONGMOVIE)"
	python3 tools/romfix.py rom/$(TARGET)-long.32x --pow2 --gran 524288
	python3 tests/verify_rom.py rom/$(TARGET)-long.32x $(OBJ)/$(TARGET).elf $(MARKERS) --min-text 2048

# ---- the hardware test ROM ----
probe:
	$(MAKE) OBJ=obj-probe TARGET=segatube-probe EXTRA_CFLAGS=-DPROBE rom/segatube-probe.32x

# ---- tests ----
host-test: tools $(TESTMOVIE)/INFO.STI $(LONGMOVIE)/INFO.STI
	python3 tests/host/test_codec.py

tests/harness: tests/harness.c
	$(HOSTCC) -O2 -std=c11 -Wall -o $@ $< -ldl -lm

check: $(ROM) test-rom test-rom-long host-test tests/harness
	@if [ -f tests/emu/picodrive_libretro.so ]; then python3 tests/run_tests.py; \
	else echo "SKIP emulator tests: put picodrive_libretro.so in tests/emu (see tests/emu/README.md)"; fi

clean:
	rm -rf $(OBJ) obj-probe rom tests/out tests/harness
