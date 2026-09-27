BITS 32

global _start
extern kmain

extern __bss_start
extern __bss_end

_start:
    mov esp, 0x90000

    ; Raw COM1 marker: proves _start itself is running before any C code,
    ; with no dependence on the serial driver being initialized.
    mov dx, 0x3F8
    mov al, '['
    out dx, al
    mov al, 'e'
    out dx, al
    mov al, ']'
    out dx, al

    ; Zero .bss before any C code runs. The frame allocator's bitmap,
    ; the heap's block headers and the task table are all static, so
    ; they would otherwise start out holding whatever the boot sector's
    ; disk read left behind. This must run before kmain(), and before
    ; the stack is used for anything, since dword stores are the only
    ; instructions here.
    cld
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosd

    mov dx, 0x3F8
    mov al, '['
    out dx, al
    mov al, 'c'
    out dx, al
    mov al, ']'
    out dx, al

    call kmain

.hang:

    cli
    hlt
    jmp .hang
