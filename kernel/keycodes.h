#ifndef KEYCODES_H
#define KEYCODES_H

/*
 * Non-ASCII keys the keyboard driver reports to the line editor.
 *
 * Printable keys arrive as their own ASCII character. Anything that
 * has no character of its own -- arrows, Home/End, Page Up/Down,
 * Delete, and the Ctrl combinations the shell cares about -- is
 * reported as one of these values instead.
 *
 * The values start at 0x80 so they cannot collide with any byte the
 * keyboard driver produces from a scancode, and 0x80 itself is left
 * unused because that is what an unmapped scancode translates to.
 */
#define KEY_NONE       0x00

#define KEY_UP         0x81
#define KEY_DOWN       0x82
#define KEY_LEFT       0x83
#define KEY_RIGHT      0x84
#define KEY_HOME       0x85
#define KEY_END        0x86
#define KEY_PAGE_UP    0x87
#define KEY_PAGE_DOWN  0x88
#define KEY_DELETE     0x89

#define KEY_CTRL_C     0x8A
#define KEY_CTRL_D     0x8B
#define KEY_CTRL_L     0x8C
#define KEY_CTRL_U     0x8D

#define KEY_TAB        '\t'

#endif
