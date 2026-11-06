#ifndef BRAD_ASM_H
#define BRAD_ASM_H

#include "bradisa.h"
#include <stddef.h>

#define BRAD_ASM_MAX_ERROR   256
#define BRAD_ASM_MAX_SYMBOLS 1024
#define BRAD_ASM_MAX_OUTPUT  65536

struct brad_as_symbol {
    char   name[64];
    uint32_t addr;
};

struct brad_as_error {
    int      line;
    char     msg[BRAD_ASM_MAX_ERROR];
};

struct brad_as_output {
    uint32_t         code[BRAD_ASM_MAX_OUTPUT];
    unsigned         count;
    unsigned         entry_point;
    struct brad_as_symbol symbols[BRAD_ASM_MAX_SYMBOLS];
    unsigned         nsymbols;
    struct brad_as_error  error;
    int              ok;
};

int brad_as_assemble(const char *source, struct brad_as_output *out);

#endif
