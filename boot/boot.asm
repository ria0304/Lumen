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

     ; Choose a read strategy.
     ;
     ; Hard disks are read with the LBA extended read (AH=0x42).
     ; Floppies are read with plain CHS (AH=0x02): SeaBIOS' floppy
     ; INT 13h handler rejects extended-read packets, so an LBA-only
     ; loader prints DISK_ERROR on every floppy boot. Geometry comes
     ; from AH=0x08, falling back to the standard 1.44 MB layout.
    cmp byte [boot_drive], 0x80
    jae .use_lba

    mov byte [use_chs], 1

    mov ah, 0x08
    mov dl, [boot_drive]
    int 0x13
    jc .floppy_defaults

    xor ch, ch
    mov cl, [spt]            ; stash sectors-per-track
    and cl, 0x3F             ; low 6 bits carry the count
    jz .floppy_defaults
    mov [spt], cl

    mov cl, ch
    inc cl
    mov [num_tracks], cl

    xor ch, ch
    mov cl, dh
    inc cl
    mov [num_heads], cl
    jmp .geometry_done

.floppy_defaults:
    mov word [spt], 18
    mov byte [num_heads], 2
    mov byte [num_tracks], 80

.geometry_done:
.use_lba:

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
    jne .do_chs

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
    jc .load_lba_failed

    jmp .load_advanced

.do_chs:
    call chs_read
    jc disk_error
    jmp .load_advanced

     ; An extended read can fail on media that is otherwise fine.
     ; Fall back to CHS once, then give up if that fails too.
.load_lba_failed:
    cmp byte [lba_tried_chs], 0
    jne disk_error
    mov byte [lba_tried_chs], 1

    cmp byte [boot_drive], 0x80
    jae disk_error          ; hard disk: CHS needs geometry we never read

    mov byte [use_chs], 1
    mov word [spt], 18
    mov byte [num_heads], 2
    mov byte [num_tracks], 80

    mov word [dest_seg], 0x1000
    mov word [dest_off], 0x0000
    mov word [lba], 1
    jmp .load_loop

.load_advanced:
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

 ; Read the sector at 'lba' into ES:BX using a CHS read (AH=0x02).
 ; Leaves the carry flag from INT 13h intact for the caller.
 ;
 ;   lba -> (cylinder, head, sector) via the geometry probed at start.
 ;   Bit 7 of CL carries the cylinder's high bits; the 1.44 MB layout
 ;   only reaches cylinder 79, so that stays clear.
chs_read:
    pusha
    push es

    mov ax, [lba]
    xor dx, dx
    div word [spt]              ; ax = cyl*heads + head, dx = sector index
    mov bl, dl                  ; stash sector index

    xor dx, dx
    div byte [num_heads]        ; ax = cylinder, dx = head
    mov ch, al                  ; cylinder, low 8 bits
    mov dh, dl                  ; head

    mov cl, bl
    inc cl                      ; sectors are 1-based

    mov bx, [dest_off]
    mov es, [dest_seg]
    mov dl, [boot_drive]
    mov ax, 0x0201              ; AH = 0x02 read, AL = 1 sector
    int 0x13

    pop es
    popa
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
lba_tried_chs db 0
spt           dw 18
num_heads     db 2
num_tracks    db 80

loading_msg    db "Loading kernel...", 0
read_ok_msg    db " READ_OK", 0
a20_ok_msg     db " A20_OK", 0
gdt_ok_msg     db " GDT_OK", 0
pm_msg         db " PM_START", 0
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

gdt_user_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11111010b
    db 11001111b
    db 0x00

gdt_user_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11110010b
    db 11001111b
    db 0x00

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

times 510 - ($ - $$) db 0
dw 0xAA55