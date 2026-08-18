# Build ReC98 PC-98 game engine components for PC-9821 Linux using libpc98

CC       := zig cc
CXX      := zig c++
OUT      ?= ../../build/tools
TARGET   := $(OUT)/rec98-demo
MUSIC    := $(OUT)/pc98-music-room
BATTLE   := $(OUT)/pc98-boss-battle
TH01     := $(OUT)/th01
TH02     := $(OUT)/th02
TH03     := $(OUT)/th03
TH04     := $(OUT)/th04
TH05     := $(OUT)/th05
LAUNCHER := $(OUT)/pc98-touhou

ZFLAGS   := -target x86-linux-musl -mcpu=i486 -Os -static -s -w -flto \
		-ffunction-sections -fdata-sections
LDFLAGS  := -Wl,--gc-sections -flto
CFLAGS   := $(ZFLAGS) -I.. -I.
CXXFLAGS := $(ZFLAGS) -I.. -I. -fno-exceptions -fno-rtti

all: $(TARGET) $(MUSIC) $(BATTLE) $(TH01) $(TH02) $(TH03) $(TH04) $(TH05) $(LAUNCHER)

$(TARGET): touhou_demo.cpp ../build/opna.o ../build/opna_hw_c.o
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ touhou_demo.cpp ../build/opna.o ../build/opna_hw_c.o

$(MUSIC): music_room.cpp ../build/opna.o ../build/opna_hw_c.o
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ music_room.cpp ../build/opna.o ../build/opna_hw_c.o

$(BATTLE): boss_battle.cpp ../build/opna.o ../build/opna_hw_c.o
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ boss_battle.cpp ../build/opna.o ../build/opna_hw_c.o

$(TH01): th01_game.cpp ../build/opna.o ../build/opna_hw_c.o
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ th01_game.cpp ../build/opna.o ../build/opna_hw_c.o

$(TH02): th02_game.cpp ../build/opna.o ../build/opna_hw_c.o
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ th02_game.cpp ../build/opna.o ../build/opna_hw_c.o

$(TH03): th03_game.cpp ../build/opna.o ../build/opna_hw_c.o
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ th03_game.cpp ../build/opna.o ../build/opna_hw_c.o

$(TH04): th04_game.cpp ../build/opna.o ../build/opna_hw_c.o
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ th04_game.cpp ../build/opna.o ../build/opna_hw_c.o

$(TH05): th05_game.cpp ../build/opna.o ../build/opna_hw_c.o
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ th05_game.cpp ../build/opna.o ../build/opna_hw_c.o

$(LAUNCHER): touhou_collection.cpp ../build/opna.o ../build/opna_hw_c.o
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ touhou_collection.cpp ../build/opna.o ../build/opna_hw_c.o

clean:
	rm -f $(TARGET) $(MUSIC) $(BATTLE) $(TH01) $(TH02) $(TH03) $(TH04) $(TH05) $(LAUNCHER)

.PHONY: all clean
