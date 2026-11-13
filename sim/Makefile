# ReChord desktop simulator: the firmware's own UI, library, file system, queue and player layer,
# built for a PC with SDL2. Only the hardware drivers are replaced (sim/*.c).
#
#   make -C sim                       # -> build/sim/rechord_sim  (BOARD=8g by default)
#   make -C sim BOARD=512m            # -> build/sim/rechord_sim_512m
#   sim/mkimage.sh ~/Music build/sim/internal.img
#   build/sim/rechord_sim build/sim/internal.img
#
# Needs: a C compiler, SDL2 (libsdl2-dev), and ffmpeg/ffprobe on PATH for audio.

ROOT    := ..
FW      := $(ROOT)/firmware
OUT     := $(ROOT)/build/sim
BOARD   ?= 8g
BOARD_ID := $(if $(filter 8g,$(BOARD)),1,2)
# objects per board (BOARD is a -D)
OBJ     := $(OUT)/obj-$(BOARD)

CC      ?= cc
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)

# The firmware is written for a 32-bit Cortex-M3 and built with lenient warnings (ap.mk). On a
# 64-bit PC the dangerous ones (a pointer through an int, a function used without a prototype)
# are errors here, so the simulator never runs code that would silently truncate a pointer.
CFLAGS  ?= -O1 -g
CFLAGS  += -std=gnu11 -D_DEFAULT_SOURCE -DRECHORD_SIM -DRECHORD_AP_BUILD -DBOARD=$(BOARD_ID) \
           -Wall -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
           -Wno-unused-but-set-variable -Wno-missing-braces -Wno-format-truncation \
           -Wno-stringop-truncation -Wno-misleading-indentation -Wno-address-of-packed-member \
           -Wno-builtin-declaration-mismatch -Wno-char-subscripts -Wno-pointer-sign \
           -Werror=implicit-function-declaration -Werror=int-conversion -MMD -MP \
           -Werror=pointer-to-int-cast -Werror=int-to-pointer-cast -Werror=incompatible-pointer-types \
           -Werror=return-type \
           -fcommon -pthread \
           -I$(FW) -I$(FW)/drivers -I$(FW)/services -I$(FW)/player -I$(FW)/ui -I$(FW)/theme -I. \
           -include sim_protos.h $(SDL_CFLAGS)

# Firmware sources used as they are (ap.mk lists, minus the drivers and the core-B audio engine).
FW_SRCS := \
  startup/ap_ui.c startup/ap_bg.c \
  ui/ui.c ui/frame.c ui/nav.c ui/screens.c ui/eq_page.c ui/lib_ui.c \
  drivers/display.c drivers/font.c drivers/font8x8.c drivers/panel_init_rknanoc.c \
  drivers/text.c drivers/icons.c drivers/fonts/fonts_s8.c drivers/fonts/icons_data.c \
  drivers/fonts/fonts_blob.c \
  services/log.c services/input.c services/scratch.c services/fs_target.c services/fs_write_ext.c \
  services/sdcard.c services/screenshot.c services/tags.c services/dirlist.c services/queue.c \
  services/config.c services/keyseq.c \
  services/lib_coll.c services/lib_io.c services/lib_sort.c services/lib_scan.c services/lib_dir.c \
  services/lib_build.c services/lib_idx.c services/lib_fsx.c services/lib_job.c services/lib_m3u.c \
  services/lib_tags.c services/lib_plat.c \
  theme/theme.c theme/themes_builtin.c \
  player/player_audio.c \
  audio/mp3_info.c audio/seek.c audio/mp4.c audio/ape_info.c audio/asf.c

# The simulator's own files.
SIM_SRCS := sim_main.c sim_hw.c sim_storage.c sim_audio.c sim_ovl.c

FW_OBJS  := $(patsubst %.c,$(OBJ)/fw/%.o,$(FW_SRCS))
SIM_OBJS := $(patsubst %.c,$(OBJ)/sim/%.o,$(SIM_SRCS))
# Two firmware files need a small change to build for a PC; it is made on a copy at build time
# (the firmware source is never edited):
#   startup/ap_ovl.c  -- an overlay header stores the link's ABI address in a uint32_t, which is
#                        not a constant on a 64-bit host: the copy stores 0 (sim_ovl.c ignores it)
#   drivers/panel_io_sim.c -- its keys, clock and delay are fixed values for the tests; renamed
#                        here so sim_hw.c gives the live ones (the keyboard, the real clock)
GEN_OBJS := $(OBJ)/gen/ap_ovl.o $(OBJ)/gen/panel_io_sim.o

BIN := $(OUT)/rechord_sim$(if $(filter 8g,$(BOARD)),,_$(BOARD))

all: $(BIN)

$(BIN): $(FW_OBJS) $(SIM_OBJS) $(GEN_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(SDL_LIBS) -lm -lpthread
	@echo "built $@ (BOARD=$(BOARD))"

$(OBJ)/fw/%.o: $(FW)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/sim/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/gen/ap_ovl.c: $(FW)/startup/ap_ovl.c
	@mkdir -p $(dir $@)
	sed 's/(uint32_t)__ovl_abi/0u/' $< > $@

$(OBJ)/gen/ap_ovl.o: $(OBJ)/gen/ap_ovl.c
	$(CC) $(CFLAGS) -I$(FW)/startup -c $< -o $@

$(OBJ)/gen/panel_io_sim.o: $(FW)/drivers/panel_io_sim.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Dkeys_adc=psim_keys_adc -Dkeys_power=psim_keys_power -Dadc_value=psim_adc_value \
	    -Dpanel_io_now_us=psim_now_us -Dpanel_io_now_ms=psim_now_ms -Dpanel_io_delay_ms=psim_delay_ms \
	    -Dboard_power_off=psim_board_power_off -Dbacklight_set=psim_backlight_set \
	    -Dbacklight_on=psim_backlight_on -Dpanel_io_select=psim_panel_io_select \
	    -Dpanel_io_poll=psim_panel_io_poll -c $< -o $@

clean:
	rm -rf $(OUT)/fw $(OUT)/sim $(OUT)/gen $(BIN)

.PHONY: all clean
