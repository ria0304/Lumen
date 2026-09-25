#ifndef LINE_EDITOR_H
#define LINE_EDITOR_H

#include <stdint.h>

#define LINE_BUFFER_SIZE 128

void line_editor_init(void);
void line_editor_handle_char(char c);
const char *line_editor_get_buffer(void);
uint32_t line_editor_length(void);
const char *line_editor_get_submitted(void);

#endif
