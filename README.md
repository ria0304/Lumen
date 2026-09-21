# Lumen

An x86 operating system written from the ground up in C and Assembly — including a custom bootloader,
kernel, interrupt/exception handling, and memory management, developed and tested in QEMU on Ubuntu.
Filesystem, multitasking, and a shell are planned as the project progresses.

> 🚧 **Status: early development.** See the checklist below for current progress.

---

## Screenshots / Demo

*(Add a screenshot or GIF here once the kernel boots and prints something — this is the first thing
anyone looking at the repo will see.)*

---

## What's Implemented

- [ ] Bootable boot sector (real mode → protected mode)
- [ ] Minimal C kernel entry point
- [ ] VGA text-mode output driver
- [ ] Global Descriptor Table (GDT)
- [ ] Interrupt Descriptor Table (IDT) + exception handlers
- [ ] PIC remapping + hardware interrupts
- [ ] Timer interrupt (PIT)
- [ ] Keyboard driver
- [ ] Basic memory allocator (bump allocator)
- [ ] Filesystem
- [ ] Processes / basic multitasking
- [ ] Shell
- [ ] Utilities / basic applications
- [ ] GUI (stretch goal)

---

## Building and Running

### Requirements

```bash
sudo apt install build-essential nasm qemu-system-x86 gdb xorriso grub-pc-bin grub-common
```

### Build

```bash
make
```

### Run in QEMU

```bash
./run.sh
```

or directly:

```bash
qemu-system-x86_64 -drive format=raw,file=build/lumen.bin
```

---

## Project Structure

```
lumen/
├── boot/           # Boot sector, real mode → protected mode switch
├── kernel/         # C kernel: VGA, IDT, IRQ, keyboard, memory
├── linker.ld       # Linker script
├── Makefile        # Build automation
├── run.sh          # QEMU launch script
└── docs/           # Screenshots, notes, design docs
```

---

## Notes / What I'm Learning

*(Running notes on what was tricky, what clicked, and any good resources — fill this in as you go.
This section is what makes the repo read as understanding rather than copied code.)*

-

---

## Roadmap

This project follows a staged roadmap, roughly:

| Stage | Status |
|---|---|
| Boot + kernel foundation | 🚧 in progress |
| Keyboard + interrupts + memory basics | ⬜ planned |
| Filesystem + storage | ⬜ planned |
| Processes + system calls + shell | ⬜ planned |
| Utilities + stability | ⬜ planned |
| GUI foundations (stretch) | ⬜ stretch goal |

Full timeline target: through April 2027, gradual hobby-project pace. Partial completion (boot + kernel +
input + basic memory + shell) already counts as a successful outcome — GUI is a stretch, not a requirement.

---

## Safety Note

Everything here runs inside QEMU as a virtual machine on top of Ubuntu. The host OS (Ubuntu/Windows
dual-boot) is never touched, and the physical disk is never written to directly.

---

## License

MIT — see [LICENSE](LICENSE).
