/*
 * sysmem.c - newlib heap backend. The heap is the region between the end of
 * .bss and the reserved stack (see _user_heap_stack in the linker script).
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

extern uint8_t _end;      /* start of the heap (linker symbol) */
extern uint8_t _estack;   /* top of RAM */
extern uint32_t _Min_Stack_Size;

void *_sbrk(ptrdiff_t incr)
{
  static uint8_t *heap_end = &_end;
  const uint8_t *limit = &_estack - (uintptr_t)&_Min_Stack_Size;

  if (heap_end + incr > limit)
  {
    errno = ENOMEM;
    return (void *)-1;
  }

  uint8_t *prev = heap_end;
  heap_end += incr;
  return prev;
}
