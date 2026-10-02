CC = gcc
LD = ld
ASM = nasm

CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib \
         -Wall -Wextra -Wno-error -c
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

BUILD = build
BUILD_TEST = build-test

BOOT_ASM = boot/boot.asm

# All kernel headers, so that editing any of them rebuilds every
# object that includes it. Without this the build links against stale
# objects and silently succeeds against an interface that no longer
# matches -- which is exactly what happened when the filesystem API
# changed shape.
KERNEL_HDRS = $(wildcard kernel/*.h)

KERNEL_OBJS = $(BUILD)/entry.o $(BUILD)/usermode.o $(BUILD)/isr.o $(BUILD)/gdt_flush.o $(BUILD)/gdt.o $(BUILD)/tss_load.o $(BUILD)/tss.o $(BUILD)/ring3probe.o \
              $(BUILD)/kernel.o $(BUILD)/console.o $(BUILD)/serial.o $(BUILD)/idt.o $(BUILD)/pic.o $(BUILD)/pit.o $(BUILD)/keyboard.o $(BUILD)/heap.o \
              $(BUILD)/line_editor.o $(BUILD)/shell.o $(BUILD)/task.o $(BUILD)/scheduler.o $(BUILD)/task_demo.o $(BUILD)/paging.o $(BUILD)/frame.o $(BUILD)/ring3.o $(BUILD)/syscall.o \
              $(BUILD)/ata.o $(BUILD)/rtc.o $(BUILD)/kmem.o $(BUILD)/fs.o $(BUILD)/loader.o $(BUILD)/gui.o $(BUILD)/settings.o $(BUILD)/users.o $(BUILD)/net.o $(BUILD)/pkg.o $(BUILD)/proc.o $(BUILD)/klog.o $(BUILD)/cron.o $(BUILD)/gfx.o $(BUILD)/nic.o $(BUILD)/uaccess.o $(BUILD)/sha256.o $(BUILD)/elfprog.o $(BUILD)/ring3test.o

KERNEL_ELF = $(BUILD)/kernel.elf
KERNEL_BIN = $(BUILD)/kernel.bin
IMG = $(BUILD)/lumen.img
DISK = $(BUILD)/disk.img

.PHONY: all clean run test disk-reset

all: $(IMG) $(DISK)

$(BUILD):
	mkdir -p $(BUILD)

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

$(BUILD)/ring3probe.o: kernel/ring3probe.asm | $(BUILD)
	$(ASM) -f elf32 $< -o $@

$(BUILD)/%.o: kernel/%.c $(KERNEL_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

$(KERNEL_BIN): $(KERNEL_ELF)
	objcopy -O binary $< $@

# The boot sector's sector count is derived from the real kernel size
# instead of being hand-maintained. KERNEL_SECTORS is a recursive
# variable, so $(shell ...) runs when the recipe below is expanded --
# which is after KERNEL_BIN has been built, as a prerequisite of this
# target. The extra sector is slack, and the check under the recipe
# turns a would-be silent truncation into a loud build failure.
KERNEL_SECTORS = $(shell echo $$(( ($$(stat -c%s $(KERNEL_BIN) 2>/dev/null || echo 0) + 511) / 512 + 1 )))

$(BUILD)/boot.bin: $(BOOT_ASM) $(KERNEL_BIN) | $(BUILD)
	@cap=$$(( $(KERNEL_SECTORS) * 512 )); \
	 size=$$(stat -c%s $(KERNEL_BIN)); \
	 if [ $$size -gt $$cap ]; then \
	   echo "ERROR: kernel ($$size bytes) exceeds boot sector capacity ($$cap bytes)."; \
	   echo "       Bump BOOT_MAX_SECTORS in boot/boot.asm or shrink the kernel."; \
	   exit 1; \
	 fi; \
	 echo "  BOOT    kernel $$size bytes -> $(KERNEL_SECTORS) sectors ($$cap byte capacity)"
	$(ASM) -f bin -DKERNEL_SECTORS=$(KERNEL_SECTORS) $< -o $@

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

# ---------------------------------------------------------------------------
# Host-side LumenFS v2 formatter. Shares kernel/fs_format.h with the
# kernel so images it writes mount verbatim. Produces tools/mkfs,
# which is git-ignored (see .gitignore).
# ---------------------------------------------------------------------------
MKFS = tools/mkfs

$(MKFS): tools/mkfs.c kernel/fs_format.h
	cc -O2 -Wall -o $@ tools/mkfs.c

.PHONY: mkfs
mkfs: $(MKFS)

# ---------------------------------------------------------------------------
# make test
#
# Builds a separate copy of the kernel with -DLUMEN_AUTOEXIT and runs it
# headless, mirroring the whole boot log over COM1. The kernel exits QEMU
# on its own once the boot self-tests are done, so this is fast and needs
# no timeout. A fresh disk is used every time so results don't depend on
# whatever the last interactive session left behind.
#
# The fresh disk is formatted with mkfs before boot. Without that the
# kernel mounts an unformatted volume, skips the whole LumenFS
# self-test, and 'make test' reports success while testing none of
# the filesystem.
# ---------------------------------------------------------------------------
# The sub-make runs with BUILD=$(BUILD_TEST), so its $(DISK) target --
# $(BUILD)/disk.img -- is exactly this path. No separate rule is
# needed; declaring one would just collide with it.
TEST_DISK = $(BUILD_TEST)/disk.img
TEST_LOG = $(BUILD_TEST)/boot.log

# The number of self-tests a healthy run must report. Anything less
# means something skipped silently, which used to read as success: with
# ATA DMA enabled the filesystem silently failed to mount, six tests
# skipped, and 'make test' still printed PASS. A skip is only ever
# acceptable when a test genuinely cannot run (no drive, unformatted
# disk), and that has to be noticed rather than absorbed, so the
# expected count is asserted rather than trusting the failure count.
EXPECTED_SELFTESTS = 28
.PHONY: test
test: $(MKFS)
	@rm -f $(TEST_DISK)
	@$(MAKE) --no-print-directory BUILD=$(BUILD_TEST) \
	   CFLAGS="$(CFLAGS) -DLUMEN_AUTOEXIT" \
	   $(BUILD_TEST)/lumen.img $(TEST_DISK)
	@echo "  TEST    booting headless, capturing serial output..."
	@./$(MKFS) $(TEST_DISK) 8192 >/dev/null
	@timeout 60 qemu-system-i386 -display none -serial stdio \
	    -boot order=a \
	    -drive file=$(BUILD_TEST)/lumen.img,format=raw,if=floppy \
	    -drive file=$(TEST_DISK),format=raw,if=ide \
	    -netdev user,id=n0 -device rtl8139,netdev=n0 \
	    -device isa-debug-exit \
	    < /dev/null > $(TEST_LOG) 2>&1; true
	@grep -E 'SELFTEST' $(TEST_LOG) || true
	@echo ""
	@if grep -q 'SELFTEST-SUMMARY pass=' $(TEST_LOG) && \
	    ! grep -qE 'SELFTEST [A-Z]+ FAIL' $(TEST_LOG); then \
	   _got=$$(grep -o 'pass=[0-9]*' $(TEST_LOG) | head -1 | cut -d= -f2); \
	   if [ "$$_got" -lt $(EXPECTED_SELFTESTS) ]; then \
	     echo "  TEST    FAIL -- $$_got of $(EXPECTED_SELFTESTS) self-tests ran;"; \
	     echo "                  the rest skipped. See $(TEST_LOG)"; \
	     grep -E 'SELFTEST [A-Z]+ SKIP' $(TEST_LOG) || true; \
	     exit 1; \
	   fi; \
	   echo "  TEST    PASS -- pass=$$_got failures=0"; \
	 else \
	   echo "  TEST    FAIL -- see $(TEST_LOG)"; exit 1; \
	 fi

clean:
	rm -rf $(BUILD) $(BUILD_TEST) tools/mkfs tools/*.o

# Interactive boot. The floppy carries the boot sector plus kernel;
# the IDE disk carries LumenFS data and is deliberately the same
# persistent file across runs, so files survive a reboot.
run: $(IMG) $(DISK)
	@echo "  RUN     Lumen (Ctrl-A X in QEMU to quit)"
	qemu-system-i386 -display curses \
	    -boot order=a \
	    -drive file=$(IMG),format=raw,if=floppy \
	    -drive file=$(DISK),format=raw,if=ide \
	    -serial stdio \
	    $(QEMUFLAGS)
