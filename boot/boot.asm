BITS 16
ORG 0x7C00

CODE_SEG equ 0x08
DATA_SEG equ 0x10

BOOT_MAX_SECTORS equ 384

%ifndef KERNEL_SECTORS
    KERNEL_SECTORS equ 120
%endif

%if KERNEL_SECTORS > BOOT_MAX_SECTORS
    %error "Kernel is larger than BOOT_MAX_SECTORS (384). Raise the limit or shrink the kernel."
%endif

start:
    cli

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov [boot_drive], dl

    mov si, loading_msg
    call print

    ; Floppies are read with plain CHS (AH=0x02), hard disks with the
    ; LBA extended read (AH=0x42). SeaBIOS rejects extended-read
    ; packets on floppies, so an LBA-only loader reports DISK_ERROR on
    ; every floppy boot. The image is always a 1.44 MB floppy.
    cmp byte [boot_drive], 0x80
    jae .read_lba
    mov byte [use_chs], 1

.read_lba:
    mov word [dest_seg], 0x1000
    mov word [dest_off], 0x0000
    mov word [lba], 1
    mov word [remaining], KERNEL_SECTORS

    mov ah, 0
    mov dl, [boot_drive]
    int 0x13

.load_loop:
    xor ax, ax
    mov ds, ax

    cmp byte [use_chs], 0
    jne .chs

    mov byte [dap_size], 16
    mov byte [dap_rsvd], 0
    mov word [dap_count], 1
    mov ax, [dest_off]
    mov [dap_off], ax
    mov ax, [dest_seg]
    mov [dap_seg], ax
    mov ax, [lba]
    mov [dap_lba], ax
    mov word [dap_lba+2], 0
    mov word [dap_lba+4], 0
    mov word [dap_lba+6], 0

    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, dap_size
    int 0x13
    jc disk_error

    jmp .advanced

.chs:
    call chs_read
    jc disk_error

.advanced:
    mov ax, [dest_off]
    add ax, 512
    jnc .no_seg_carry
    add word [dest_seg], 0x1000
.no_seg_carry:
    mov [dest_off], ax

    inc word [lba]
    dec word [remaining]
    jnz .load_loop

.load_done:
    xor ax, ax
    mov ds, ax
    mov si, read_ok_msg
    call print

    in al, 0x92
    or al, 00000010b
    out 0x92, al

    mov si, a20_ok_msg
    call print

    cli
    lgdt [gdt_descriptor]

    mov si, gdt_ok_msg
    call print

    mov si, pm_msg
    call print

    mov eax, cr0
    or eax, 0x00000001
    mov cr0, eax

    jmp dword CODE_SEG:protected_mode_entry


disk_error:
    mov al, ah
    call print_hex8
    mov al, 0x0D
    call print_char
    mov al, 0x0A
    call print_char
    mov si, disk_error_msg
    call print
    jmp halt


print_char:
    mov ah, 0x0E
    int 0x10
    ret


print_hex8:
    push ax
    shr al, 4
    call .nib
    pop ax
    and al, 0x0F
    call .nib
    ret
.nib:
    cmp al, 9
    jbe .d
    add al, 7
.d:
    add al, '0'
    call print_char
    ret


print:
.next:
    lodsb
    test al, al
    jz .done

    mov ah, 0x0E
    int 0x10

    jmp .next

.done:
    ret


halt:
    cli
    hlt
    jmp halt


BITS 32

protected_mode_entry:

    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov esp, 0x90000

    mov word [0xB8000], 0x0741
    mov word [0xB8002], 0x0742
    mov word [0xB8004], 0x0743

    mov eax, 0x10000
    jmp dword CODE_SEG:0x10000


BITS 16

; Read the sector at 'lba' into ES:[dest_off] via a CHS read (AH=0x02).
; 1.44 MB geometry: 80 cylinders x 2 heads x 18 sectors. Returns with
; the carry flag from INT 13h intact. The progress printer uses
; DS:SI (lodsb), so leaving ES clobbered is harmless.
chs_read:
    mov ax, [lba]
    xor dx, dx
    div word [spt]           ; ax = cyl*2 + head, dx = sector index
    mov bl, dl
    xor dx, dx
    div word [spt2]          ; divide by heads (2)
    mov ch, al
    mov dh, dl
    mov cl, bl
    inc cl                   ; sectors are 1-based
    mov bx, [dest_off]
    mov es, [dest_seg]
    mov dl, [boot_drive]
    mov ax, 0x0201           ; AH=02 read one sector
    int 0x13
    ret

boot_drive db 0
dap_size   db 16
dap_rsvd   db 0
dap_count  dw 1
dap_off    dw 0
dap_seg    dw 0
dap_lba    dq 0
dest_seg   dw 0x0000
dest_off   dw 0x0000
lba        dw 0x0000
remaining  dw 0x0000
use_chs       db 0
spt           dw 18
spt2          dw 2

loading_msg    db "Load", 0
read_ok_msg    db " OK", 0
a20_ok_msg     db " A20", 0
gdt_ok_msg     db " GDT", 0
pm_msg         db " PM", 0
disk_error_msg db " DISK_ERROR", 0


gdt_start:

gdt_null:
    dq 0

gdt_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 11001111b
    db 0x00

gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

times 510 - ($ - $$) db 0
dw 0xAA55