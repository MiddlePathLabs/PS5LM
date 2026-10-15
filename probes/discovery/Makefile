ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined: source scripts/env.sh)
endif

CFLAGS := -Wall -Wextra -Werror -O2 -g
LDLIBS := -lkernel_sys -lpthread

discovery.elf: main.c ../ps5test.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

.PHONY: clean
clean:
	rm -f discovery.elf
