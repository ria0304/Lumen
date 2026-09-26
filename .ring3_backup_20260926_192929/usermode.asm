BITS 32

global enter_user_mode

extern ring3_entered

%define USER_CODE_SELECTOR 0x1B
%define USER_DATA_SELECTOR 0x23

%define USER_CODE_ADDRESS  0x01000000
%define USER_STACK_TOP     0x01002000

enter_user_mode:

    cli

    /*
     * iret frame for CPL3:
     *
     *   SS
     *   ESP
     *   EFLAGS
     *   CS
     *   EIP
     */
    push dword USER_DATA_SELECTOR
    push dword USER_STACK_TOP

    pushfd
    pop eax

    /*
     * IF=1
     * IOPL=0
     */
    or eax, 0x00000200

    push eax

    push dword USER_CODE_SELECTOR
    push dword USER_CODE_ADDRESS

    iret
