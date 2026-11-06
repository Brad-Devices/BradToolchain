#include "ptx.h"

#include <ctype.h>
#include <string.h>

/* PTX is line-oriented and its identifiers absorb dots, which is what makes
 * `ld.global.f32`, `%tid.x` and `$L__BB0_2` single tokens.  A token that
 * starts with a digit (or a sign followed by a digit) is a number; PTX uses
 * +4 / -4 only inside address brackets, so swallowing the sign is safe.
 */

static int ident_start(int c)
{
    return isalpha((unsigned char)c) || c == '_' || c == '%' || c == '$';
}

static int ident_char(int c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '%' || c == '$' ||
           c == '.';
}

static void set_text(struct cvt_token *t, const char *s, size_t n)
{
    if (n >= sizeof(t->text))
        n = sizeof(t->text) - 1;
    memcpy(t->text, s, n);
    t->text[n] = '\0';
}

void cvt_lex_init(struct cvt_lexer *lx, const char *src)
{
    memset(lx, 0, sizeof(*lx));
    lx->p = src;
    lx->line = 1;
}

void cvt_lex_next(struct cvt_lexer *lx)
{
    for (;;) {
        char c = *lx->p;

        if (c == ' ' || c == '\t' || c == '\r') {
            lx->p++;
            continue;
        }
        if (c == '/' && lx->p[1] == '/') {
            while (*lx->p && *lx->p != '\n')
                lx->p++;
            continue;
        }
        if (c == '/' && lx->p[1] == '*') {
            lx->p += 2;
            while (*lx->p && !(lx->p[0] == '*' && lx->p[1] == '/')) {
                if (*lx->p == '\n')
                    lx->line++;
                lx->p++;
            }
            if (*lx->p)
                lx->p += 2;
            continue;
        }
        break;
    }

    memset(&lx->t, 0, sizeof(lx->t));
    lx->t.line = lx->line;

    char c = *lx->p;
    if (c == '\0') {
        lx->t.kind = TOK_EOF;
        return;
    }
    if (c == '\n') {
        lx->p++;
        lx->line++;
        lx->t.kind = TOK_NL;
        return;
    }

    /* Number: 123, 0x1f, 0f3F800000, -4, +4, 1.0, 1e-3, .5 */
    if (isdigit((unsigned char)c) ||
        ((c == '-' || c == '+') && isdigit((unsigned char)lx->p[1])) ||
        (c == '.' && isdigit((unsigned char)lx->p[1]))) {
        const char *start = lx->p;
        while (isalnum((unsigned char)*lx->p) || *lx->p == '.' ||
               *lx->p == '+' || *lx->p == '-' || *lx->p == '_')
            lx->p++;
        lx->t.kind = TOK_NUMBER;
        set_text(&lx->t, start, (size_t)(lx->p - start));
        return;
    }

    /* Identifier: starts with letter, _, %, $, or a dot followed by one. */
    if (ident_start((unsigned char)c) ||
        (c == '.' && (isalpha((unsigned char)lx->p[1]) || lx->p[1] == '_'))) {
        const char *start = lx->p;
        while (ident_char((unsigned char)*lx->p))
            lx->p++;
        lx->t.kind = TOK_IDENT;
        set_text(&lx->t, start, (size_t)(lx->p - start));
        return;
    }

    if (c == '"') {
        lx->p++;
        const char *start = lx->p;
        while (*lx->p && *lx->p != '"' && *lx->p != '\n')
            lx->p++;
        set_text(&lx->t, start, (size_t)(lx->p - start));
        if (*lx->p == '"')
            lx->p++;
        lx->t.kind = TOK_STRING;
        return;
    }

    /* Everything else is a one-character punctuation token. */
    lx->t.kind = TOK_PUNCT;
    lx->t.text[0] = c;
    lx->t.text[1] = '\0';
    lx->p++;
}
