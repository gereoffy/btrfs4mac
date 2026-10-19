# libbtrfsro + command line tools
#
#   make            build build/libbtrfsro.a and build/btrfsro
#   make test       run the test scripts against the images in teszt-*/
#   make fuzz       build the sanitizer-instrumented corruption fuzzer
#   make clean

CC      ?= clang
ARCHS   ?=
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
           -Wno-missing-field-initializers $(ARCHS)
CPPFLAGS += -Icore/include -D_FILE_OFFSET_BITS=64
ZSTD_CPPFLAGS = -DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0 -DDEBUGLEVEL=0 \
                -DZSTD_NO_INTRINSICS=0
LDLIBS  += -lz

BUILD   := build

CORE_SRC := $(wildcard core/src/*.c)
ZSTD_SRC := $(wildcard core/vendor/zstd/common/*.c) \
            $(wildcard core/vendor/zstd/decompress/*.c)

CORE_OBJ := $(CORE_SRC:%.c=$(BUILD)/%.o)
ZSTD_OBJ := $(ZSTD_SRC:%.c=$(BUILD)/%.o)

LIB      := $(BUILD)/libbtrfsro.a
TOOL     := $(BUILD)/btrfsro

all: $(LIB) $(TOOL)

$(BUILD)/core/vendor/%.o: core/vendor/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ZSTD_CPPFLAGS) $(CFLAGS) -w -c $< -o $@

$(BUILD)/%.o: %.c $(wildcard core/src/*.h) core/include/btrfsro.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(LIB): $(CORE_OBJ) $(ZSTD_OBJ)
	@rm -f $@
	ar rcs $@ $^

$(TOOL): $(BUILD)/tools/btrfsro.o $(LIB)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

HASHTEST := $(BUILD)/hash-vectors

$(HASHTEST): tests/hash-vectors.c $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

test: all $(HASHTEST)
	tests/run-tests.sh

# Corruption fuzzer, built with address and undefined behaviour sanitizers
SAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
             -fno-omit-frame-pointer
FUZZ := $(BUILD)/fuzz

fuzz: $(FUZZ)

$(FUZZ): tests/fuzz.c $(CORE_SRC) $(ZSTD_SRC) $(wildcard core/src/*.h) core/include/btrfsro.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(ZSTD_CPPFLAGS) -std=gnu11 -w $(SAN_FLAGS) $(ARCHS) \
		tests/fuzz.c $(CORE_SRC) $(ZSTD_SRC) $(LDLIBS) -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all test fuzz clean
