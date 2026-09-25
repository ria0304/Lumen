BITS 32

global enter_user_mode
global user_mode_start

enter_user_mode:
    cli

    push dword 0x23
    push dword 0x80000
    pushfd
    push dword 0x1B
    push dword user_mode_start

    iret

user_mode_start:
    int 0x80

    ; This instruction is privileged at Ring 3.
    ; It should trigger General Protection Fault (#GP).
    cli

.user_loop:
    jmp .user_loop
