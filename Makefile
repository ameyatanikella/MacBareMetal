# m5-baremetal top-level build.
#
#   make setup       install the toolchain (Homebrew + rustup)
#   make fetch       clone m1n1 at the pinned commit and apply our hooks
#   make test        host tests of the portable code (no hardware needed)
#   make sim         render the screen output to out/sim-*/ (layout check)
#   make image       build out/m5rt.bin = m1n1 + runtime
#   make stage DEST="/Volumes/<test volume data>"   copy image + install script
#
# Risk-ladder switches for `make image` (all off by default):
#   RT_WDT_GUARD=1    arm the watchdog as a hang guard        (rung 2)
#   RT_NVME_PROBE=1   run m1n1's NVMe driver, read-only       (rung 3)
#   RT_GPU_PROBE=1    guarded reads of GPU registers           (rung 4)
#   RT_GPU_POWER=1    also power the GPU domain on and off     (rung 5)
# Timing: RT_HOLD_S=12 RT_FINAL_HOLD_S=45 RT_FATAL_HOLD_S=90

M1N1_COMMIT := 6ee64bdd0cd028f0e6446606cc4ef4c299142584
M1N1_URL    := https://github.com/AsahiLinux/m1n1.git
M1N1_DIR    := vendor/m1n1

BREW  ?= /opt/homebrew
GMAKE ?= $(BREW)/opt/make/libexec/gnubin/make
export PATH := $(BREW)/opt/rustup/bin:$(PATH)

RT_DIR      := $(abspath rt)
RT_BUILD_ID := $(shell date +%Y%m%d-%H%M%S)

RT_VARS := RT_DIR=$(RT_DIR) RT_BUILD_ID=$(RT_BUILD_ID)
RT_VARS += $(foreach v,RT_WDT_GUARD RT_NVME_PROBE RT_GPU_PROBE RT_GPU_POWER RT_EXIT_TO_PROXY RT_HOLD_S RT_FINAL_HOLD_S \
	RT_FATAL_HOLD_S RT_ARENA_MB,$(if $($(v)),$(v)=$($(v))))

.PHONY: all setup fetch test sim image stage clean distclean

all: test image

setup:
	scripts/setup-toolchain.sh

fetch:
	scripts/fetch-m1n1.sh $(M1N1_URL) $(M1N1_COMMIT) $(M1N1_DIR)

test:
	@mkdir -p out
	cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
		-Irt tests/host/test_arena.c rt/rt_arena.c -o out/test_arena
	out/test_arena

# Render the on-screen output on the host (layout and paging only).
SIM_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
	-Itests/host/sim/stubs -Irt -Iout/sim-inc \
	-DRT_HOLD_S=3 -DRT_FINAL_HOLD_S=3 -DRT_FATAL_HOLD_S=3 -DRT_ARENA_MB=256 \
	-DRT_BUILD_ID='"sim"'
sim:
	@mkdir -p out/sim-retina out/sim-1080p out/sim-inc
	@# m1n1's real fb.h and xnuboot.h, copied so that their "types.h" resolves
	@# to the host stand-in instead of m1n1's freestanding one.
	cp $(M1N1_DIR)/src/fb.h $(M1N1_DIR)/src/xnuboot.h out/sim-inc/
	cc $(SIM_CFLAGS) tests/host/sim/sim_ui.c rt/rt_ui.c -o out/sim_ui
	out/sim_ui $(M1N1_DIR)/font/font.bin $(M1N1_DIR)/font/font_retina.bin out/sim-retina 3024 1964 1
	out/sim_ui $(M1N1_DIR)/font/font.bin $(M1N1_DIR)/font/font_retina.bin out/sim-1080p 1920 1080 0
	python3 tests/host/sim/ppm2png.py out/sim-retina 2
	python3 tests/host/sim/ppm2png.py out/sim-1080p 1

image:
	@test -f $(M1N1_DIR)/Makefile || { echo "run 'make fetch' first"; exit 1; }
	@test -x $(GMAKE) || { echo "GNU make 4 not found at $(GMAKE); run 'make setup'"; exit 1; }
	$(GMAKE) -C $(M1N1_DIR) -j8 $(RT_VARS)
	@mkdir -p out
	cp $(M1N1_DIR)/build/m1n1.bin out/m5rt.bin
	@echo "$(RT_BUILD_ID)" > out/m5rt.build-id
	@echo
	@echo "built out/m5rt.bin  build id $(RT_BUILD_ID)"
	@shasum -a 256 out/m5rt.bin

stage:
	@test -n "$(DEST)" || { echo 'usage: make stage DEST="/Volumes/<test volume data>"'; exit 1; }
	scripts/stage.sh "$(DEST)"

clean:
	rm -rf out
	-test -d $(M1N1_DIR) && $(GMAKE) -C $(M1N1_DIR) clean

distclean: clean
	rm -rf vendor
