#include "syscall.h"
#include "console.h"

void syscall_handler(void)
{
    console_info("System call received");
}
