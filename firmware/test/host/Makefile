# Host (PC) tests for fx algorithms. Uses the MSYS2 UCRT64 gcc by default.
# Run from Git Bash with MSYS2 make (needs TMP set to a writable dir):
#   PATH=/c/msys64/ucrt64/bin:$PATH mingw32-make            all test_*.c
#   PATH=/c/msys64/ucrt64/bin:$PATH mingw32-make test_mod   one group
# WAV renders go to out/ for listening.

HOST_CC ?= /c/msys64/ucrt64/bin/gcc
FW      := ../..
CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Wdouble-promotion -Wshadow -Werror \
           -I$(FW)/Core/Inc -I.
LDLIBS  := -lm

COMMON  := harness.c $(FW)/Core/Src/sample_data.c

TESTS   := $(basename $(wildcard test_*.c))

# Each test_<group>.c is linked with Core/Src/algos/algo_<group>.c;
# test_chain links the whole engine (every algorithm + chain + registry).
ENGINE  := $(wildcard $(FW)/Core/Src/algos/*.c) $(FW)/Core/Src/fx_engine.c \
           $(FW)/Core/Src/fx_params.c $(FW)/Core/Src/fx_algo_registry.c
group    = $(patsubst test_%,%,$(1))
algo_src = $(if $(filter chain,$(call group,$(1))),$(ENGINE),$(FW)/Core/Src/algos/algo_$(call group,$(1)).c)

.PHONY: all clean $(TESTS)
all: $(TESTS)

$(TESTS): %: build/%.exe | out
	./build/$@.exe

build/%.exe: %.c $(COMMON) harness.h | build
	PATH=/c/msys64/ucrt64/bin:$$PATH $(HOST_CC) $(CFLAGS) -o $@ $< $(COMMON) $(call algo_src,$*) $(LDLIBS)

build out:
	mkdir -p $@

clean:
	rm -rf build out
