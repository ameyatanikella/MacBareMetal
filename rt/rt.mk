# Included by vendor/m1n1/Makefile when RT_DIR is set (see patches/).
# Adds the runtime's objects to the m1n1 link and defines RT_RUNTIME, which
# switches on the three hooks in main.c and utils.c.

RT_SRCS := rt_main.c rt_ui.c rt_arena.c rt_probe.c rt_wdt.c rt_diag.c

OBJECTS += $(patsubst %.c,rt/%.o,$(RT_SRCS))
CFG += RT_RUNTIME

# Build-time switches. Each one moves a rung up the risk ladder in
# docs/TESTING.md; all are off for the first boot.
RT_HOLD_S       ?= 12
RT_FINAL_HOLD_S ?= 45
RT_FATAL_HOLD_S ?= 90
RT_ARENA_MB     ?= 256
RT_BUILD_ID     ?= unstamped

RT_CFLAGS := -I$(RT_DIR) \
	-DRT_HOLD_S=$(RT_HOLD_S) \
	-DRT_FINAL_HOLD_S=$(RT_FINAL_HOLD_S) \
	-DRT_FATAL_HOLD_S=$(RT_FATAL_HOLD_S) \
	-DRT_ARENA_MB=$(RT_ARENA_MB) \
	-DRT_BUILD_ID='"$(RT_BUILD_ID)"'

ifeq ($(RT_WDT_GUARD),1)
RT_CFLAGS += -DRT_WDT_GUARD
endif
ifeq ($(RT_NVME_PROBE),1)
RT_CFLAGS += -DRT_NVME_PROBE
endif
ifeq ($(RT_GPU_PROBE),1)
RT_CFLAGS += -DRT_GPU_PROBE
endif
ifeq ($(RT_GPU_POWER),1)
RT_CFLAGS += -DRT_GPU_PROBE -DRT_GPU_POWER
endif
ifeq ($(RT_EXIT_TO_PROXY),1)
RT_CFLAGS += -DRT_EXIT_TO_PROXY
endif

# This file is included above m1n1's own first rule, so keep `all` the default.
.DEFAULT_GOAL := all

# Rebuild the runtime objects whenever the switches change.
.PHONY: rt-force
build/rt/.flags: rt-force
	$(QUIET)mkdir -p build/rt
	$(QUIET)echo '$(RT_CFLAGS)' | cmp -s - $@ || echo '$(RT_CFLAGS)' > $@

build/rt/%.o: $(RT_DIR)/%.c $(wildcard $(RT_DIR)/*.h) build/rt/.flags build-tag build-cfg
	$(QUIET)echo "  CC RT $@"
	$(QUIET)mkdir -p $(DEPDIR)
	$(QUIET)mkdir -p "$(dir $@)"
	$(QUIET)$(CC) -c $(CFLAGS) $(RT_CFLAGS) -o $@ $<
