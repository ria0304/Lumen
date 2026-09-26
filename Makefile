CC = gcc
LD = ld
ASM = nasm

CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib \
         -Wall -Wextra -c
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

BUILD = build

BOOT_ASM = boot/boot.asm

KERNEL_OBJS = $(BUILD)/entry.o $(BUILD)/usermode.o $(BUILD)/isr.o $(BUILD)/gdt_flush.o $(BUILD)/gdt.o $(BUILD)/tss_load.o $(BUILD)/tss.o \
              $(BUILD)/kernel.o $(BUILD)/console.o $(BUILD)/idt.o $(BUILD)/pic.o $(BUILD)/pit.o $(BUILD)/keyboard.o $(BUILD)/heap.o \
              $(BUILD)/line_editor.o $(BUILD)/shell.o $(BUILD)/task.o $(BUILD)/scheduler.o $(BUILD)/syscall.o

KERNEL_ELF = $(BUILD)/kernel.elf
KERNEL_BIN = $(BUILD)/kernel.bin
IMG = $(BUILD)/lumer.img

.PHONY: all clean run

all: $(IMG)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.bin: $(BOOT_ASM) | $(BUILD)
	$(ASM) -f bin $< -o $@

$(BUILD)/usermode.o: kernel/usermode.asm | $(BUILD)
	$(ASM) -f elf32 $< -o $@

$(BUILD)/entry.o: kernel/entry.asm | $(BUILD)
	$(ASM) -f elf32 $< -o $@

$(BUILD)/gdt_flush.o: kernel/gdt_flush.asm | $(BUILD)
	$(ASM) -f elf32 $< -o $@

$(BUILD)/tss_load.o: kernel/tss_load.asm | $(BUILD)
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
