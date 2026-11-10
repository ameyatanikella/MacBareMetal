# GPU reverse-engineering experiments that run under macOS, on this Mac.
#
#   make            build the tools
#   make corpus     compile and run every kernel variant, write data/corpus.json
#   make analyze    bit-correlation report, written to data/analysis.txt
#   make trace      log the driver's kernel calls for one run
#
# Nothing here needs elevated rights or changes any system setting.

CC := clang

all: out/mtlrun out/iowrap.dylib

out/mtlrun: metal/mtlrun.m
	@mkdir -p out
	$(CC) -fobjc-arc -O1 -Wall -framework Foundation -framework Metal $< -o $@

out/iowrap.dylib: wrap/iowrap.c
	@mkdir -p out
	$(CC) -dynamiclib -O1 -Wall -framework IOKit $< -o $@

corpus: out/mtlrun
	python3 tools/experiment.py

analyze:
	python3 tools/analyze.py

trace: all
	DYLD_INSERT_LIBRARIES=out/iowrap.dylib IOWRAP_LOG=out/trace_add.txt out/mtlrun kernels/add_u32.metal --n 8
	python3 tools/trace_summary.py out/trace_add.txt

clean:
	rm -rf out

.PHONY: all corpus analyze trace clean
