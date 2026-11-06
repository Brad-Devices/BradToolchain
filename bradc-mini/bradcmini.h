#ifndef BRADC_MINI_H
#define BRADC_MINI_H

#include "brad/bradasm.h"

/* Compile a C-subset source string to a BradISA V2 machine image.
 *
 * On success fills *out with the assembled words and returns 0.
 * On failure returns -1 and writes a human-readable message to err.
 *
 * The subset (v0):
 *   types:    int, void
 *   decls:    functions with up to 8 int params
 *   stmts:    int x = expr;  x = expr;  if/else  while  return expr;  { }
 *   exprs:    literals, variables, calls, + - * / % & | ^ << >>,
 *             == != < > <= >=, unary - and !, parentheses
 *   no pointers, arrays, structs, globals, preprocessor, or floating point.
 */
int bradcmini_compile(const char *src, struct brad_as_output *out,
                      char *err, size_t errsz);

/* Compile to assembly text instead of machine code.  The caller owns the
 * returned buffer (free() it).  Returns NULL on failure. */
char *bradcmini_to_asm(const char *src, char *err, size_t errsz);

#endif
