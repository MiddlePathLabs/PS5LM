PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined: source scripts/env.sh)
endif

# One ELF per title, the id fixed at build time. main.c refuses any title
# that is not a PS5LM one.
ELFS := launch-PPSA99040.elf launch-PPSA99041.elf

CFLAGS := -Wall -Wextra -Werror -O2 -g
LDLIBS := -lSceSystemService -lSceUserService

all: $(ELFS)

launch-%.elf: main.c ../ps5test.h
	$(CC) $(CFLAGS) -DTITLE_ID='"$*"' -o $@ $< $(LDLIBS)

clean:
	rm -f $(ELFS)

.PHONY: all clean
