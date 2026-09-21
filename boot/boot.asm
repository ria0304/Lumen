BITS 16
ORG 0x7C00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl

    mov si, loading_msg

.print:
    lodsb
    test al, al
    jz .read_kernel
    mov ah, 0x0E
    int 0x10
    jmp .print

.read_kernel:
    xor ax, ax
    mov es, ax

    mov ah, 0x02
    mov al, 0x01
    mov ch, 0x00
    mov cl, 0x02
    mov dh, 0x00
    mov dl, [boot_drive]
    mov bx, 0x1000

    int 0x13
    jc disk_error

    mov si, success_msg

.success:
    lodsb
    test al, al
    jz halt
    mov ah, 0x0E
    int 0x10
    jmp .success

disk_error:
    mov si, error_msg

.error:
    lodsb
    test al, al
    jz halt
    mov ah, 0x0E
    int 0x10
    jmp .error

halt:
    cli
    hlt
    jmp halt

boot_drive db 0

loading_msg db "Loading kernel...", 0
success_msg db " Kernel loaded!", 0
error_msg   db " Disk read error!", 0

times 510-($-$$) db 0
dw 0xAA55
