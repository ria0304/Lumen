#ifndef LOADER_H
#define LOADER_H

/*
 * Bridges LumenFS to task_create_user_program(): reads a named
 * file's raw bytes and runs them directly as Ring 3 machine code
 * (no ELF, no relocation, no headers -- the file's contents ARE
 * the flat binary, loaded at TASK_RING3_CODE_VA). This is enough
 * to run a hand-assembled or objcopy -O binary program, and no
 * more; a real loader would need to understand an actual
 * executable format instead.
 *
 * Returns the new task's id on success, or -1 if the file doesn't
 * exist, is empty, or is bigger than one page (4096 bytes).
 */
int loader_spawn(const char *filename);

#endif
