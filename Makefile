CC = gcc
LD = ld
ASM = nasm

CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib \
         -Wall -Wextra -c
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

BUILD = build

BOOT_ASM = boot/boot.asm

KERNEL_OBJS = $(BUILD)/entry.o $(BUILD)/isr.o \
              $(BUILD)/kernel.o $(BUILD)/idt.o $(BUILD)/pic.o $(BUILD)/pit.o $(BUILD)/keyboard.o $(BUILD)/heap.o

KERNEL_ELF = $(BUILD)/kernel.elf
KERNEL_BIN = $(BUILD)/kernel.bin
IMG = $(BUILD)/lumer.img

.PHONY: all clean run

all: $(IMG)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.bin: $(BOOT_ASM) | $(BUILD)
	$(ASM) -f bin $< -o $@

$(BUILD)/entry.o: kernel/entry.asm | $(BUILD)
	$(ASM) -f elf32 $< -o $@

$(BUILD)/isr.o: kernel/isr.asm | $(BUILD)
	$(ASM) -f elf32 $< -o $@

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

$(KERNEL_BIN): $(KERNEL_ELF)
	objcopy -O binary $< $@

$(IMG): $(BUILD)/boot.bin $(KERNEL_BIN)
	cat $(BUILD)/boot.bin $(KERNEL_BIN) > $@
	truncate -s 1440k $@ || true

clean:
	rm -rf $(BUILD)

run: $(IMG)
	qemu-system-i386 -fda $(IMG)
