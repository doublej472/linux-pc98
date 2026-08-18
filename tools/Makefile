# Build the PC-98 userspace tools for the low-memory i486 targets.
#
# pc98snd is the real-time hardware player: the pmdmini (PMDWin) sequencer,
# vendored under pmdmini/, drives the PC-9801-86 through a hardware OPNA
# backend (pmdmini/hw/opna_hw.cpp + opna_hw.c) instead of the ymfm software
# emulator.
#
# The binaries are built with the Zig toolchain targeting x86-linux-musl at
# i486: Zig bundles a musl libc and libc++ compiled for the exact CPU, so the
# whole static binary is free of post-486 instructions (the host's 32-bit
# glibc/libstdc++ are built with SSE, which SIGILLs on the Celeron).

CC      := zig cc
CXX     := zig c++
OUT     ?= ../build/tools
TARGET  := $(OUT)/pc98snd
TIMESYNC:= $(OUT)/pc98-timesync
DEMO    := $(OUT)/pc98-demo-game
TEST_SUITE := $(OUT)/test_libpc98
BENCH   := $(OUT)/pc98-benchmark
TRIDENT_BENCH := $(OUT)/pc98-trident-bench

SEQ    := pmdmini/src/pmdwin
YM     := pmdmini/src/ymfm
HW     := pmdmini/hw
BUILD  := build

ZFLAGS   := -target x86-linux-musl -mcpu=i486 -Os -static -s -w -flto \
		-ffunction-sections -fdata-sections
LDFLAGS  := -Wl,--gc-sections -flto
CFLAGS   := $(ZFLAGS)
CXXFLAGS := $(ZFLAGS) -DPMDMINI_HW_BACKEND -fno-exceptions -fno-rtti
INCLUDES := -I. -I$(SEQ) -I$(YM) -I$(HW)

OBJS := \
	$(BUILD)/pc98snd.o \
	$(BUILD)/opna.o \
	$(BUILD)/opna_hw_c.o \
	$(BUILD)/pmd_player.o \
	$(BUILD)/opna_hw.o \
	$(BUILD)/pmdwincore.o \
	$(BUILD)/pmdwin.o \
	$(BUILD)/table.o \
	$(BUILD)/util.o \
	$(BUILD)/ppsdrv.o \
	$(BUILD)/p86drv.o \
	$(BUILD)/ppz8l.o \
	$(BUILD)/file_fmgen.o \
	$(BUILD)/sjis2utf.o

all: $(TARGET) $(TIMESYNC) $(DEMO) $(TEST_SUITE) $(BENCH) $(TRIDENT_BENCH)

$(TARGET): $(OBJS)
	mkdir -p $(OUT)
	$(CXX) $(ZFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(TIMESYNC): timesync.c
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(DEMO): demo_game.c $(BUILD)/opna.o $(BUILD)/opna_hw_c.o
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ demo_game.c $(BUILD)/opna.o $(BUILD)/opna_hw_c.o

$(TEST_SUITE): test_libpc98.c $(BUILD)/opna.o $(BUILD)/opna_hw_c.o
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ test_libpc98.c $(BUILD)/opna.o $(BUILD)/opna_hw_c.o

$(BENCH): bench.c $(BUILD)/opna.o $(BUILD)/opna_hw_c.o
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ bench.c $(BUILD)/opna.o $(BUILD)/opna_hw_c.o

$(TRIDENT_BENCH): diag/trident_bench.c
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ diag/trident_bench.c

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/pc98snd.o: pc98snd.c opna_io.h opna_hw_c.h pmd_player.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/opna.o: opna.c opna_io.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/opna_hw_c.o: opna_hw.c opna_hw_c.h opna_io.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/pmd_player.o: pmd_player.cpp pmd_player.h opna_io.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/opna_hw.o: $(HW)/opna_hw.cpp $(HW)/opna_hw.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/pmdwincore.o: $(SEQ)/pmdwincore.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/pmdwin.o: $(SEQ)/pmdwin.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/table.o: $(SEQ)/table.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/util.o: $(SEQ)/util.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/ppsdrv.o: $(SEQ)/ppsdrv.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/p86drv.o: $(SEQ)/p86drv.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/ppz8l.o: $(SEQ)/ppz8l.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/file_fmgen.o: $(YM)/file_fmgen.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/sjis2utf.o: $(YM)/sjis2utf.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

clean:
	rm -rf $(BUILD) $(TARGET) $(TIMESYNC) $(DEMO) $(TEST_SUITE) $(BENCH) $(TRIDENT_BENCH)

.PHONY: all clean
