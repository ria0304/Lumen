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
              $(BUILD)/line_editor.o $(BUILD)/shell.o $(BUILD)/task.o $(BUILD)/scheduler.o $(BUILD)/task_demo.o $(BUILD)/paging.o $(BUILD)/frame.o $(BUILD)/ring3.o $(BUILD)/syscall.o \
              $(BUILD)/ata.o $(BUILD)/fs.o $(BUILD)/loader.o

KERNEL_ELF = $(BUILD)/kernel.elf
KERNEL_BIN = $(BUILD)/kernel.bin
IMG = $(BUILD)/lumer.img
DISK = $(BUILD)/disk.img

.PHONY: all clean run

all: $(IMG) $(DISK)

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

$(BUILD)/ring3.o: kernel/ring3.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

$(KERNEL_BIN): $(KERNEL_ELF)
	objcopy -O binary $< $@

$(IMG): $(BUILD)/boot.bin $(KERNEL_BIN)
	cat $(BUILD)/boot.bin $(KERNEL_BIN) > $@
	truncate -s 1440k $@ || true

# Second, separate disk for LumenFS (kernel/fs.c, kernel/ata.c).
# Created once and left alone on rebuilds so 'format'/'write' data
# in it survives a plain 'make'; delete it yourself (or 'make
# disk-reset') to start over.
$(DISK): | $(BUILD)
	@if [ ! -f $(DISK) ]; then \
		dd if=/dev/zero of=$(DISK) bs=1024 count=4096 status=none; \
	fi

.PHONY: disk-reset
disk-reset:
	rm -f $(DISK)
	$(MAKE) $(DISK)

clean:
	rm -rf $(BUILD)

run: $(IMG) $(DISK)
	qemu-system-i386 -fda $(IMG) -hda $(DISK)
