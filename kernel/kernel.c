void kmain(void)
{
    volatile unsigned short *vga = (volatile unsigned short *)0xB8000;

    const char *message = "Lumer kernel online!";

    for (int i = 0; message[i] != '\0'; i++) {
        vga[i] = (unsigned short)message[i] | 0x0700;
    }

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
