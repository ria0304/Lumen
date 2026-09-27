#ifndef LINE_EDITOR_H
#define LINE_EDITOR_H

#include <stdint.h>

#define LINE_BUFFER_SIZE 128

/* How many submitted lines are kept for up-arrow recall. */
#define LINE_HISTORY_SIZE 16

void line_editor_init(void);

/*
 * Feed one key from the keyboard driver. Printable keys arrive as
 * their own ASCII value; everything else arrives as a KEY_* code from
 * keycodes.h, which this handles for cursor movement, history
 * recall, and the Ctrl shortcuts. Unrecognised codes are ignored
 * rather than being inserted as garbage.
 *
 * The parameter is uint8_t, not char: the KEY_* codes run from 0x81
 * to 0x8D, and a signed char cannot even hold those as switch labels
 * -- the cases would silently never match.
 */
void line_editor_handle_char(uint8_t key);

const char *line_editor_get_buffer(void);
uint32_t line_editor_length(void);
const char *line_editor_get_submitted(void);

#endif
