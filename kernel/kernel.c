#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000
#define VGA_COLOR 0x07

static int terminal_row = 0;
static int terminal_column = 0;

static volatile unsigned short *vga =
    (volatile unsigned short *)VGA_MEMORY;

static void terminal_clear(void)
{
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            int index = y * VGA_WIDTH + x;
            vga[index] = ((unsigned short)VGA_COLOR << 8) | ' ';
        }
    }

    terminal_row = 0;
    terminal_column = 0;
}

static void terminal_putchar(char c)
{
    if (c == '\n') {
        terminal_column = 0;
        terminal_row++;

        if (terminal_row >= VGA_HEIGHT)
            terminal_row = 0;

        return;
    }

    int index = terminal_row * VGA_WIDTH + terminal_column;

    vga[index] = ((unsigned short)VGA_COLOR << 8) | c;

    terminal_column++;

    if (terminal_column >= VGA_WIDTH) {
        terminal_column = 0;
        terminal_row++;

        if (terminal_row >= VGA_HEIGHT)
            terminal_row = 0;
    }
}

static void terminal_write(const char *message)
{
    for (int i = 0; message[i] != '\0'; i++) {
        terminal_putchar(message[i]);
    }
}

void kmain(void)
{
    terminal_clear();

    terminal_write("Lumer kernel online!\n");
    terminal_write("VGA text driver: OK\n");
    terminal_write("Protected mode: 32-bit\n");
    terminal_write("Day 4 milestone: COMPLETE\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
