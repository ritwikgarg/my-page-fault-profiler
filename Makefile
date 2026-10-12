CFLAGS_MODULE += -Wno-declaration-after-statement -Werror
APP_CFLAGS = -std=c11 -pipe -O2 -Werror

KERNEL_SRC := ~/linux-5.15.165
SUBDIR := $(PWD)

CC ?= gcc

.PHONY: clean

all: clean modules monitor work

obj-m:= mp3.o

modules:
	$(MAKE) -C $(KERNEL_SRC) M=$(SUBDIR) -Wall modules

monitor: monitor.c
	$(CC) $(APP_CFLAGS) $< -o $@

work: work.c
	$(CC) $(APP_CFLAGS) $< -o $@

clean:
	rm -f monitor work *~ *.ko *.o *.mod.c Module.symvers modules.order


