; Simple ELF test program for Lumen OS
; This is a minimal 32-bit ELF executable

BITS 32

section .text
global _start

_start:
    ; syscall SYS_GETPID (1)
    mov eax, 1
    int 0x80
    
    ; Exit with syscall SYS_EXIT (3)
    mov eax, 3
    int 0x80
    
    jmp $

section .data
    msg db "Hello from ELF!", 0
    msg_len equ $ - msg
