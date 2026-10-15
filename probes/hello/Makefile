PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined: source scripts/env.sh)
endif

ELFS := hello.elf hello-fail.elf hello-crash.elf hello-hang.elf

CFLAGS := -Wall -Wextra -Werror -O2 -g

all: $(ELFS)

hello.elf: main.c ../ps5test.h
	$(CC) $(CFLAGS) -DHELLO_MODE=0 -o $@ $<
hello-fail.elf: main.c ../ps5test.h
	$(CC) $(CFLAGS) -DHELLO_MODE=1 -o $@ $<
hello-crash.elf: main.c ../ps5test.h
	$(CC) $(CFLAGS) -DHELLO_MODE=2 -o $@ $<
hello-hang.elf: main.c ../ps5test.h
	$(CC) $(CFLAGS) -DHELLO_MODE=3 -o $@ $<

clean:
	rm -f $(ELFS)

test: hello.elf
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

.PHONY: all clean test
