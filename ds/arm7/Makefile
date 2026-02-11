# ARM7 Makefile
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment")
endif

include $(DEVKITARM)/ds_rules

TARGET := arm7
BUILD := build
SOURCES := .
INCLUDES := .

ARCH := -mthumb -march=armv4t -mtune=arm7tdmi

CFLAGS := -g -Wall -O2 $(ARCH) -DARM7 -fno-builtin -I$(LIBNDS)/include -I$(DEVKITPRO)/calico/include

LDFLAGS = -specs=ds_arm7.specs -g $(ARCH) -Wl,-Map,$(notdir $*).map -L$(DEVKITPRO)/libnds/lib -L$(DEVKITPRO)/calico/lib

LIBS := -lnds7 -lcalico_ds7

CC := arm-none-eabi-gcc
LD := arm-none-eabi-g++
OBJCOPY := arm-none-eabi-objcopy

all: $(TARGET).elf

$(TARGET).elf: arm7.o
	@echo "linking $(notdir $@)"
	@$(LD) $(LDFLAGS) -o $@ arm7.o $(LIBS)

arm7.o: arm7.c
	@echo "$(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -f *.o $(TARGET).elf

.PHONY: all clean
