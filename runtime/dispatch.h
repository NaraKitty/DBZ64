#ifndef DISPATCH_H
#define DISPATCH_H
#include "gba.h"

typedef struct {
    uint32_t addr;          /* guest address | 1 for Thumb */
    gba_func_t fn;
} gba_func_entry_t;

extern const gba_func_entry_t gba_func_table[];
extern const unsigned gba_func_count;

#endif
