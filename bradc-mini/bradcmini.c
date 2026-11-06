#include "bradcmini.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char last_type_struct[64];
static int nstruct_defs = 0;

/* =====================================================================
 * Lexer
 * ===================================================================== */

enum tok {
    T_EOF, T_INT, T_VOID, T_STRUCT, T_IF, T_ELSE, T_WHILE, T_RETURN,
    T_IDENT, T_NUM,
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACK, T_RBRACK, T_DOT, T_SEMI, T_COMMA,
    T_ASSIGN,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_AMP, T_PIPE, T_CARET, T_LSHIFT, T_RSHIFT,
    T_EQ, T_NE, T_LT, T_GT, T_LE, T_GE,
    T_BANG,
    T_UNKNOWN
};

struct token {
    enum tok kind;
    char text[64];
    long ival;
};

struct lexer {
    const char *p;
    int line;
    struct token tok;
};

static void lex_init(struct lexer *lx, const char *src) {
    lx->p = src;
    lx->line = 1;
    memset(&lx->tok, 0, sizeof lx->tok);
}

static void skip_ws(struct lexer *lx) {
    for (;;) {
        while (*lx->p == ' ' || *lx->p == '\t' || *lx->p == '\r' || *lx->p == '\n') {
            if (*lx->p == '\n') lx->line++;
            lx->p++;
        }
        if (lx->p[0] == '/' && lx->p[1] == '/') {
            while (*lx->p && *lx->p != '\n') lx->p++;
        } else if (lx->p[0] == '/' && lx->p[1] == '*') {
            lx->p += 2;
            while (lx->p[0] && !(lx->p[0] == '*' && lx->p[1] == '/')) {
                if (*lx->p == '\n') lx->line++;
                lx->p++;
            }
            if (lx->p[0]) lx->p += 2;
        } else {
            break;
        }
    }
}

static int is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static int is_ident_char(char c)  { return isalnum((unsigned char)c) || c == '_'; }

static void lex_next(struct lexer *lx) {
    skip_ws(lx);
    memset(&lx->tok, 0, sizeof lx->tok);
    char c = *lx->p;
    if (c == '\0') { lx->tok.kind = T_EOF; return; }
    if (c == '(') { lx->p++; lx->tok.kind = T_LPAREN; return; }
    if (c == ')') { lx->p++; lx->tok.kind = T_RPAREN; return; }
    if (c == '[') { lx->p++; lx->tok.kind = T_LBRACK; return; }
    if (c == ']') { lx->p++; lx->tok.kind = T_RBRACK; return; }
    if (c == '{') { lx->p++; lx->tok.kind = T_LBRACE; return; }
    if (c == '}') { lx->p++; lx->tok.kind = T_RBRACE; return; }
    if (c == ';') { lx->p++; lx->tok.kind = T_SEMI; return; }
    if (c == ',') { lx->p++; lx->tok.kind = T_COMMA; return; }
    if (c == '.') { lx->p++; lx->tok.kind = T_DOT; return; }
    if (c == '+') { lx->p++; lx->tok.kind = T_PLUS; return; }
    if (c == '-') { lx->p++; lx->tok.kind = T_MINUS; return; }
    if (c == '*') { lx->p++; lx->tok.kind = T_STAR; return; }
    if (c == '/') { lx->p++; lx->tok.kind = T_SLASH; return; }
    if (c == '%') { lx->p++; lx->tok.kind = T_PERCENT; return; }
    if (c == '&') {
        if (lx->p[1] == '&') { lx->p += 2; lx->tok.kind = T_AMP; /* reuse as logical-and marker handled in parser? we'll treat & as bitwise, && separate not supported v0 */ }
        else { lx->p++; lx->tok.kind = T_AMP; }
        return;
    }
    if (c == '|') {
        if (lx->p[1] == '|') { lx->p += 2; } else { lx->p++; }
        lx->tok.kind = T_PIPE;
        return;
    }
    if (c == '^') { lx->p++; lx->tok.kind = T_CARET; return; }
    if (c == '!') {
        if (lx->p[1] == '=') { lx->p += 2; lx->tok.kind = T_NE; return; }
        lx->p++; lx->tok.kind = T_BANG; return;
    }
    if (c == '=') {
        if (lx->p[1] == '=') { lx->p += 2; lx->tok.kind = T_EQ; return; }
        lx->p++; lx->tok.kind = T_ASSIGN; return;
    }
    if (c == '<') {
        if (lx->p[1] == '<') { lx->p += 2; lx->tok.kind = T_LSHIFT; return; }
        if (lx->p[1] == '=') { lx->p += 2; lx->tok.kind = T_LE; return; }
        lx->p++; lx->tok.kind = T_LT; return;
    }
    if (c == '>') {
        if (lx->p[1] == '>') { lx->p += 2; lx->tok.kind = T_RSHIFT; return; }
        if (lx->p[1] == '=') { lx->p += 2; lx->tok.kind = T_GE; return; }
        lx->p++; lx->tok.kind = T_GT; return;
    }
    if (isdigit((unsigned char)c)) {
        const char *s = lx->p;
        long v = 0;
        while (isdigit((unsigned char)*lx->p)) { v = v*10 + (*lx->p - '0'); lx->p++; }
        lx->tok.kind = T_NUM;
        lx->tok.ival = v;
        snprintf(lx->tok.text, sizeof lx->tok.text, "%ld", v);
        (void)s;
        return;
    }
    if (is_ident_start(c)) {
        const char *s = lx->p;
        size_t n = 0;
        while (is_ident_char(*lx->p) && n < sizeof lx->tok.text - 1) {
            lx->tok.text[n++] = *lx->p++;
        }
        lx->tok.text[n] = '\0';
        if (strcmp(lx->tok.text, "int") == 0) lx->tok.kind = T_INT;
        else if (strcmp(lx->tok.text, "void") == 0) lx->tok.kind = T_VOID;
        else if (strcmp(lx->tok.text, "struct") == 0) lx->tok.kind = T_STRUCT;
        else if (strcmp(lx->tok.text, "if") == 0) lx->tok.kind = T_IF;
        else if (strcmp(lx->tok.text, "else") == 0) lx->tok.kind = T_ELSE;
        else if (strcmp(lx->tok.text, "while") == 0) lx->tok.kind = T_WHILE;
        else if (strcmp(lx->tok.text, "return") == 0) lx->tok.kind = T_RETURN;
        else lx->tok.kind = T_IDENT;
        (void)s;
        return;
    }
    lx->tok.kind = T_UNKNOWN;
    snprintf(lx->tok.text, sizeof lx->tok.text, "unexpected '%c'", c);
    lx->p++;
}

/* =====================================================================
 *AST
 * ===================================================================== */

typedef enum { TY_INT, TY_PTR, TY_VOID, TY_STRUCT } CType;

typedef enum {
    EXPR_INT, EXPR_VAR, EXPR_BINOP, EXPR_UNOP, EXPR_CALL,
    EXPR_ADDR, EXPR_DEREF, EXPR_INDEX, EXPR_FIELD
} ExprKind;

typedef struct Expr {
    ExprKind kind;
    long ival;
    char name[64];
    int op;
    struct Expr *left, *right;
    struct Expr **args;
    int nargs;
} Expr;

typedef enum {
    STMT_DECL, STMT_ASSIGN, STMT_IF, STMT_WHILE,
    STMT_RETURN, STMT_EXPR, STMT_BLOCK
} StmtKind;

typedef struct Stmt {
    StmtKind kind;
    CType type;
    char name[64];
    char struct_name[64];
    int array_size;
    Expr *init;
    Expr *cond;
    struct Stmt *thenb, *elseb;
    struct Stmt *body;
    Expr *expr;
    Expr *ptr;
    Expr *target;
    struct Stmt **stmts;
    int nstmts;
} Stmt;

typedef struct Func {
    CType rettype;
    char name[64];
    char params[8][64];
    int nparams;
    Stmt *body;
} Func;

typedef struct { char name[64]; char fields[16][64]; int nfields; } StructDef;
static StructDef struct_defs[32];

typedef struct { char name[64]; long init; int has_init; int nelem; char type[64]; } Global;

typedef struct Program {
    StructDef *structs;
    int nstructs, capstructs;
    Global *globals;
    int nglobals, capglobals;
    Func **funcs;
    int nfuncs;
} Program;

/* helpers */
static Expr *new_expr(ExprKind k) {
    Expr *e = calloc(1, sizeof *e);
    e->kind = k;
    return e;
}
static Stmt *new_stmt(StmtKind k) {
    Stmt *s = calloc(1, sizeof *s);
    s->kind = k;
    return s;
}
static Func *new_func(void) {
    return calloc(1, sizeof(Func));
}

/* =====================================================================
 *Parser
 * ===================================================================== */

struct parser {
    struct lexer lx;
    struct token cur;
    char *err;
    size_t errsz;
};

static void p_error(struct parser *p, const char *msg) {
    if (p->err && p->errsz) {
        snprintf(p->err, p->errsz, "line %d: %s", p->lx.line, msg);
    }
}

static void p_advance(struct parser *p) {
    lex_next(&p->lx);
    p->cur = p->lx.tok;
}

static int p_expect(struct parser *p, enum tok k, const char *what) {
    if (p->cur.kind != k) {
        char buf[128];
        snprintf(buf, sizeof buf, "expected %s", what);
        p_error(p, buf);
        return -1;
    }
    p_advance(p);
    return 0;
}

/* forward decls */
static Expr *parse_expr(struct parser *p);
static Stmt *parse_stmt(struct parser *p);
static Stmt *parse_block(struct parser *p);

static Expr *parse_primary(struct parser *p) {
    if (p->cur.kind == T_NUM) {
        Expr *e = new_expr(EXPR_INT);
        e->ival = p->cur.ival;
        p_advance(p);
        return e;
    }
    if (p->cur.kind == T_IDENT) {
        char name[64];
        strcpy(name, p->cur.text);
        p_advance(p);
        if (p->cur.kind == T_LPAREN) {
            /* function call */
            p_advance(p);
            Expr *e = new_expr(EXPR_CALL);
            strcpy(e->name, name);
            int cap = 4, n = 0;
            Expr **args = malloc(cap * sizeof(Expr*));
            if (p->cur.kind != T_RPAREN) {
                for (;;) {
                    Expr *a = parse_expr(p);
                    if (!a) { /* error */ return NULL; }
                    if (n == cap) { cap *= 2; args = realloc(args, cap * sizeof(Expr*)); }
                    args[n++] = a;
                    if (p->cur.kind == T_COMMA) { p_advance(p); continue; }
                    break;
                }
            }
            if (p_expect(p, T_RPAREN, "')'") != 0) return NULL;
            e->args = args;
            e->nargs = n;
            return e;
        }
        Expr *e = new_expr(EXPR_VAR);
        strcpy(e->name, name);
        while (p->cur.kind == T_LBRACK) {
            p_advance(p);
            Expr *idx = parse_expr(p);
            if (p_expect(p, T_RBRACK, "']'") != 0) return NULL;
            Expr *ex = new_expr(EXPR_INDEX);
            ex->left = e;
            ex->right = idx;
            e = ex;
        }
        while (p->cur.kind == T_DOT) {
            p_advance(p);
            if (p->cur.kind != T_IDENT) { p_error(p, "expected field name"); return NULL; }
            Expr *ex = new_expr(EXPR_FIELD);
            ex->left = e;
            strcpy(ex->name, p->cur.text);
            p_advance(p);
            e = ex;
        }
        return e;
    }
    if (p->cur.kind == T_LPAREN) {
        p_advance(p);
        Expr *e = parse_expr(p);
        if (p_expect(p, T_RPAREN, "')'") != 0) return NULL;
        /* array index */
        while (p->cur.kind == T_LBRACK) {
            p_advance(p);
            Expr *idx = parse_expr(p);
            if (p_expect(p, T_RBRACK, "']'") != 0) return NULL;
            Expr *ex = new_expr(EXPR_INDEX);
            ex->left = e;
            ex->right = idx;
            e = ex;
        }
        while (p->cur.kind == T_DOT) {
            p_advance(p);
            if (p->cur.kind != T_IDENT) { p_error(p, "expected field name"); return NULL; }
            Expr *ex = new_expr(EXPR_FIELD);
            ex->left = e;
            strcpy(ex->name, p->cur.text);
            p_advance(p);
            e = ex;
        }
        return e;
    }
    p_error(p, "expected expression");
    return NULL;
}

static Expr *parse_unary(struct parser *p) {
    if (p->cur.kind == T_AMP) {
        p_advance(p);
        Expr *e = new_expr(EXPR_ADDR);
        e->left = parse_unary(p);
        return e;
    }
    if (p->cur.kind == T_STAR) {
        p_advance(p);
        Expr *e = new_expr(EXPR_DEREF);
        e->left = parse_unary(p);
        return e;
    }
    if (p->cur.kind == T_MINUS) {
        p_advance(p);
        Expr *e = new_expr(EXPR_UNOP);
        e->op = T_MINUS;
        e->left = parse_unary(p);
        return e;
    }
    if (p->cur.kind == T_BANG) {
        p_advance(p);
        Expr *e = new_expr(EXPR_UNOP);
        e->op = T_BANG;
        e->left = parse_unary(p);
        return e;
    }
    return parse_primary(p);
}

static Expr *parse_mul(struct parser *p) {
    Expr *l = parse_unary(p);
    while (l && (p->cur.kind == T_STAR || p->cur.kind == T_SLASH || p->cur.kind == T_PERCENT)) {
        int op = p->cur.kind;
        p_advance(p);
        Expr *r = parse_unary(p);
        if (!r) return NULL;
        Expr *e = new_expr(EXPR_BINOP);
        e->op = op;
        e->left = l;
        e->right = r;
        l = e;
    }
    return l;
}

static Expr *parse_add(struct parser *p) {
    Expr *l = parse_mul(p);
    while (l && (p->cur.kind == T_PLUS || p->cur.kind == T_MINUS)) {
        int op = p->cur.kind;
        p_advance(p);
        Expr *r = parse_mul(p);
        if (!r) return NULL;
        Expr *e = new_expr(EXPR_BINOP);
        e->op = op;
        e->left = l;
        e->right = r;
        l = e;
    }
    return l;
}

static Expr *parse_shift(struct parser *p) {
    Expr *l = parse_add(p);
    while (l && (p->cur.kind == T_LSHIFT || p->cur.kind == T_RSHIFT)) {
        int op = p->cur.kind;
        p_advance(p);
        Expr *r = parse_add(p);
        if (!r) return NULL;
        Expr *e = new_expr(EXPR_BINOP);
        e->op = op;
        e->left = l;
        e->right = r;
        l = e;
    }
    return l;
}

static Expr *parse_rel(struct parser *p) {
    Expr *l = parse_shift(p);
    while (l && (p->cur.kind == T_LT || p->cur.kind == T_GT ||
                 p->cur.kind == T_LE || p->cur.kind == T_GE)) {
        int op = p->cur.kind;
        p_advance(p);
        Expr *r = parse_shift(p);
        if (!r) return NULL;
        Expr *e = new_expr(EXPR_BINOP);
        e->op = op;
        e->left = l;
        e->right = r;
        l = e;
    }
    return l;
}

static Expr *parse_eq(struct parser *p) {
    Expr *l = parse_rel(p);
    while (l && (p->cur.kind == T_EQ || p->cur.kind == T_NE)) {
        int op = p->cur.kind;
        p_advance(p);
        Expr *r = parse_rel(p);
        if (!r) return NULL;
        Expr *e = new_expr(EXPR_BINOP);
        e->op = op;
        e->left = l;
        e->right = r;
        l = e;
    }
    return l;
}

static Expr *parse_and(struct parser *p) {
    Expr *l = parse_eq(p);
    while (l && p->cur.kind == T_AMP) {
        p_advance(p);
        Expr *r = parse_eq(p);
        if (!r) return NULL;
        Expr *e = new_expr(EXPR_BINOP);
        e->op = T_AMP;
        e->left = l;
        e->right = r;
        l = e;
    }
    return l;
}

static Expr *parse_xor(struct parser *p) {
    Expr *l = parse_and(p);
    while (l && p->cur.kind == T_CARET) {
        p_advance(p);
        Expr *r = parse_and(p);
        if (!r) return NULL;
        Expr *e = new_expr(EXPR_BINOP);
        e->op = T_CARET;
        e->left = l;
        e->right = r;
        l = e;
    }
    return l;
}

static Expr *parse_or(struct parser *p) {
    Expr *l = parse_xor(p);
    while (l && p->cur.kind == T_PIPE) {
        p_advance(p);
        Expr *r = parse_xor(p);
        if (!r) return NULL;
        Expr *e = new_expr(EXPR_BINOP);
        e->op = T_PIPE;
        e->left = l;
        e->right = r;
        l = e;
    }
    return l;
}

static Expr *parse_expr(struct parser *p) {
    return parse_or(p);
}

static Stmt *parse_block(struct parser *p) {
    if (p_expect(p, T_LBRACE, "'{'") != 0) return NULL;
    Stmt *blk = new_stmt(STMT_BLOCK);
    int cap = 8, n = 0;
    Stmt **stmts = malloc(cap * sizeof(Stmt*));
    while (p->cur.kind != T_RBRACE && p->cur.kind != T_EOF) {
        Stmt *s = parse_stmt(p);
        if (!s) return NULL;
        if (n == cap) { cap *= 2; stmts = realloc(stmts, cap * sizeof(Stmt*)); }
        stmts[n++] = s;
    }
    if (p_expect(p, T_RBRACE, "'}'") != 0) return NULL;
    blk->stmts = stmts;
    blk->nstmts = n;
    return blk;
}

static Stmt *parse_stmt(struct parser *p) {
    if (p->cur.kind == T_LBRACE) {
        return parse_block(p);
    }
    if (p->cur.kind == T_IF) {
        p_advance(p);
        if (p_expect(p, T_LPAREN, "'('") != 0) return NULL;
        Expr *cond = parse_expr(p);
        if (!cond) return NULL;
        if (p_expect(p, T_RPAREN, "')'") != 0) return NULL;
        Stmt *th = parse_stmt(p);
        if (!th) return NULL;
        Stmt *st = new_stmt(STMT_IF);
        st->cond = cond;
        st->thenb = th;
        if (p->cur.kind == T_ELSE) {
            p_advance(p);
            st->elseb = parse_stmt(p);
            if (!st->elseb) return NULL;
        }
        return st;
    }
    if (p->cur.kind == T_WHILE) {
        p_advance(p);
        if (p_expect(p, T_LPAREN, "'('") != 0) return NULL;
        Expr *cond = parse_expr(p);
        if (!cond) return NULL;
        if (p_expect(p, T_RPAREN, "')'") != 0) return NULL;
        Stmt *body = parse_stmt(p);
        if (!body) return NULL;
        Stmt *st = new_stmt(STMT_WHILE);
        st->cond = cond;
        st->body = body;
        return st;
    }
    if (p->cur.kind == T_RETURN) {
        p_advance(p);
        Stmt *st = new_stmt(STMT_RETURN);
        if (p->cur.kind != T_SEMI) {
            st->expr = parse_expr(p);
            if (!st->expr) return NULL;
        }
        if (p_expect(p, T_SEMI, "';'") != 0) return NULL;
        return st;
    }
    if (p->cur.kind == T_INT || p->cur.kind == T_VOID || p->cur.kind == T_STRUCT) {
        /* declaration or function? handled in parse_function; here only local decl */
        CType t;
        if (p->cur.kind == T_INT) t = TY_INT;
        else if (p->cur.kind == T_VOID) t = TY_VOID;
        else t = TY_STRUCT;
        p_advance(p);
        if (t == TY_STRUCT) {
            if (p->cur.kind != T_IDENT) { p_error(p, "expected struct name"); return NULL; }
            /* consume struct name */
            char sname[64];
            strcpy(sname, p->cur.text);
            strcpy(last_type_struct, sname);
            p_advance(p);
        }
        while (p->cur.kind == T_STAR) { p_advance(p); t = TY_PTR; }
        if (p->cur.kind != T_IDENT) { p_error(p, "expected identifier"); return NULL; }
        char name[64];
        strcpy(name, p->cur.text);
        p_advance(p);
        int asz = 1;
        if (p->cur.kind == T_LBRACK) {
            p_advance(p);
            if (p->cur.kind != T_NUM) { p_error(p, "array size must be constant"); return NULL; }
            asz = (int)p->cur.ival;
            if (asz < 1 || asz > 64) { p_error(p, "array size out of range (1..64)"); return NULL; }
            p_advance(p);
            if (p_expect(p, T_RBRACK, "']'") != 0) return NULL;
        }
        Stmt *st = new_stmt(STMT_DECL);
        st->type = t;
        strcpy(st->name, name);
        st->array_size = asz;
        st->struct_name[0] = '\0';
        if (t == TY_STRUCT) {
            strcpy(st->struct_name, last_type_struct);
            for (int si = 0; si < nstruct_defs; si++)
                if (strcmp(struct_defs[si].name, last_type_struct) == 0) {
                    st->array_size = struct_defs[si].nfields;
                    break;
                }
        }
        if (p->cur.kind == T_ASSIGN) {
            p_advance(p);
            st->init = parse_expr(p);
            if (!st->init) return NULL;
        }
        if (p_expect(p, T_SEMI, "';'") != 0) return NULL;
        return st;
    }
    /* assignment or expr statement */
    Expr *e = parse_expr(p);
    if (!e) return NULL;
    if (p->cur.kind == T_ASSIGN) {
        if (e->kind != EXPR_VAR && e->kind != EXPR_DEREF && e->kind != EXPR_INDEX && e->kind != EXPR_FIELD) {
            p_error(p, "invalid assignment target"); return NULL;
        }
        p_advance(p);
        Expr *rhs = parse_expr(p);
        if (!rhs) return NULL;
        Stmt *st = new_stmt(STMT_ASSIGN);
        st->target = e;
        st->expr = rhs;
        if (e->kind == EXPR_VAR) strcpy(st->name, e->name);
        if (p_expect(p, T_SEMI, "';'") != 0) return NULL;
        return st;
    }
    Stmt *st = new_stmt(STMT_EXPR);
    st->expr = e;
    if (p_expect(p, T_SEMI, "';'") != 0) return NULL;
    return st;
}

static Program *parse_program(struct parser *p) {
    Program *prog = calloc(1, sizeof *prog);
    nstruct_defs = 0;
    last_type_struct[0] = '\0';
    int fcap = 4, fn = 0;
    Func **fs = malloc(fcap * sizeof(Func*));
    int gcap = 8, gn = 0;
    Global *gs = malloc(gcap * sizeof(Global));
    while (p->cur.kind != T_EOF) {
        if (p->cur.kind == T_STRUCT) {
            /* struct definition: struct Name { fields }; */
            p_advance(p); /* struct */
            if (p->cur.kind != T_IDENT) { p_error(p, "expected struct name"); return NULL; }
            if (prog->nstructs == prog->capstructs) {
                prog->capstructs = prog->capstructs ? prog->capstructs * 2 : 4;
                prog->structs = realloc(prog->structs, prog->capstructs * sizeof(StructDef));
            }
            StructDef *sd = &prog->structs[prog->nstructs++];
            memset(sd, 0, sizeof *sd);
            strcpy(sd->name, p->cur.text);
            p_advance(p);
            if (p_expect(p, T_LBRACE, "'{'") != 0) return NULL;
            while (p->cur.kind != T_RBRACE && p->cur.kind != T_EOF) {
                /* Fields are int-only in v0.  Nested structs and pointer
                   fields are rejected rather than silently flattened. */
                if (p->cur.kind != T_INT) {
                    p_error(p, "struct fields must be int");
                    return NULL;
                }
                p_advance(p); /* int */
                if (p->cur.kind == T_STAR) {
                    p_error(p, "struct fields must be int");
                    return NULL;
                }
                if (p->cur.kind != T_IDENT) { p_error(p, "expected field name"); return NULL; }
                if (sd->nfields >= 16) { p_error(p, "too many struct fields"); return NULL; }
                strcpy(sd->fields[sd->nfields], p->cur.text);
                sd->nfields++;
                p_advance(p);
                if (p_expect(p, T_SEMI, "';'") != 0) return NULL;
            }
            if (p_expect(p, T_RBRACE, "'}'") != 0) return NULL;
            if (p->cur.kind == T_SEMI) p_advance(p);
            if (nstruct_defs >= 32) { p_error(p, "too many struct definitions"); return NULL; }
            struct_defs[nstruct_defs++] = *sd;
            continue;
        }
        if (p->cur.kind != T_INT && p->cur.kind != T_VOID && p->cur.kind != T_STRUCT) {
            p_error(p, "expected type");
            return NULL;
        }
        /* peek: if next is ident then '(' -> function */
        /* we need to check without consuming; parse type first */
        int is_void = (p->cur.kind == T_VOID);
        int is_struct = (p->cur.kind == T_STRUCT);
        char struct_type_name[64] = "";
        if (is_struct) {
            p_advance(p); /* struct */
            if (p->cur.kind != T_IDENT) { p_error(p, "expected struct name"); return NULL; }
            strcpy(struct_type_name, p->cur.text);
            p_advance(p);
            strcpy(last_type_struct, struct_type_name);
        } else {
            p_advance(p); /* int/void */
        }
        if (p->cur.kind != T_IDENT) { p_error(p, "expected identifier"); return NULL; }
        char name[64];
        strcpy(name, p->cur.text);
        p_advance(p);
        if (p->cur.kind == T_LPAREN) {
            /* function: rebuild a Func by re-parsing is awkward; do manual */
            /* We consumed type and name; need to parse params and body */
            Func *f = new_func();
            f->rettype = is_void ? TY_VOID : (is_struct ? TY_STRUCT : TY_INT);
            strcpy(f->name, name);
            if (p_expect(p, T_LPAREN, "'('") != 0) return NULL;
            int n = 0;
            if (p->cur.kind != T_RPAREN) {
                for (;;) {
                    int isv = (p->cur.kind == T_VOID);
                    int isst = (p->cur.kind == T_STRUCT);
                    if (p->cur.kind != T_INT && p->cur.kind != T_VOID && p->cur.kind != T_STRUCT) {
                        p_error(p, "expected type"); return NULL;
                    }
                    p_advance(p); /* type */
                    if (isst) {
                        if (p->cur.kind != T_IDENT) { p_error(p, "expected struct name"); return NULL; }
                        p_advance(p);
                    }
                    if (p->cur.kind == T_STAR) p_advance(p); /* pointer */
                    if (isv && p->cur.kind == T_RPAREN) break;
                    if (p->cur.kind != T_IDENT) { p_error(p, "expected param name"); return NULL; }
                    if (n >= 8) { p_error(p, "too many params"); return NULL; }
                    strcpy(f->params[n], p->cur.text);
                    n++;
                    p_advance(p);
                    if (p->cur.kind == T_COMMA) { p_advance(p); continue; }
                    break;
                }
            }
            f->nparams = n;
            if (p_expect(p, T_RPAREN, "')'") != 0) return NULL;
            f->body = parse_block(p);
            if (!f->body) return NULL;
            if (fn == fcap) { fcap *= 2; fs = realloc(fs, fcap * sizeof(Func*)); }
            fs[fn++] = f;
        } else {
            /* global declaration: name already consumed; handle optional init */
            if (gn == gcap) { gcap *= 2; gs = realloc(gs, gcap * sizeof(Global)); }
            strcpy(gs[gn].name, name);
            gs[gn].has_init = 0;
            gs[gn].init = 0;
            gs[gn].nelem = 1;
            gs[gn].type[0] = '\0';
            if (is_struct) {
                strcpy(gs[gn].type, struct_type_name);
                for (int si = 0; si < nstruct_defs; si++)
                    if (strcmp(struct_defs[si].name, struct_type_name) == 0) {
                        gs[gn].nelem = struct_defs[si].nfields;
                        break;
                    }
            }
            if (p->cur.kind == T_LBRACK) {
                p_advance(p);
                if (p->cur.kind != T_NUM) { p_error(p, "array size must be constant"); return NULL; }
                gs[gn].nelem = (int)p->cur.ival;
                if (gs[gn].nelem < 1 || gs[gn].nelem > 1024) { p_error(p, "global array size out of range (1..1024)"); return NULL; }
                p_advance(p);
                if (p_expect(p, T_RBRACK, "']'") != 0) return NULL;
            }
            if (p->cur.kind == T_ASSIGN) {
                p_advance(p);
                if (p->cur.kind == T_NUM) {
                    gs[gn].init = p->cur.ival;
                    gs[gn].has_init = 1;
                    p_advance(p);
                } else if (p->cur.kind == T_MINUS) {
                    p_advance(p);
                    if (p->cur.kind != T_NUM) { p_error(p, "expected number"); return NULL; }
                    gs[gn].init = -p->cur.ival;
                    gs[gn].has_init = 1;
                    p_advance(p);
                } else {
                    p_error(p, "global initializer must be constant");
                    return NULL;
                }
            }
            if (p_expect(p, T_SEMI, "';'") != 0) return NULL;
            gn++;
        }
    }
    prog->funcs = fs;
    prog->nfuncs = fn;
    prog->globals = gs;
    prog->nglobals = gn;
    prog->capglobals = gcap;
    return prog;
}

/* =====================================================================
 *Codegen
 * ===================================================================== */

struct codegen {
    char *buf;
    size_t len, cap;
    /* current function locals */
    char locals[64][64];
    int local_off[64];
    int local_nelem[64];
    int local_is_array[64];
    int nlocals;
    int frame_size;
    int saved_lr_off;
    int spill_off;
    int label_counter;
    Global *globals;
    int nglobals;
    int *gaddr;
    StructDef *structs;
    int nstructs;
    char local_type[64][64];
};

static void cg_emit(struct codegen *g, const char *s) {
    size_t l = strlen(s);
    while (g->len + l + 2 >= g->cap) {
        g->cap = g->cap ? g->cap * 2 : 4096;
        g->buf = realloc(g->buf, g->cap);
    }
    memcpy(g->buf + g->len, s, l);
    g->len += l;
    g->buf[g->len++] = '\n';
    g->buf[g->len] = '\0';
}

static void cg_printf(struct codegen *g, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    cg_emit(g, tmp);
}

/* find local offset; -1 if not found */
static int find_local(struct codegen *g, const char *name) {
    for (int i = 0; i < g->nlocals; i++)
        if (strcmp(g->locals[i], name) == 0) return g->local_off[i];
    return -1;
}
static int find_local_idx(struct codegen *g, const char *name) {
    for (int i = 0; i < g->nlocals; i++)
        if (strcmp(g->locals[i], name) == 0) return i;
    return -1;
}
static int find_global(struct codegen *g, const char *name) {
    for (int i = 0; i < g->nglobals; i++)
        if (strcmp(g->globals[i].name, name) == 0) return i;
    return -1;
}

/* collect local variable names from a statement */
static void collect_locals_stmt(struct codegen *g, Stmt *s) {
    if (!s) return;
    switch (s->kind) {
        case STMT_DECL:
            strcpy(g->locals[g->nlocals], s->name);
            {
                int asz = s->array_size ? s->array_size : 1;
                g->local_nelem[g->nlocals] = asz;
                g->local_is_array[g->nlocals] = (asz > 1);
                if (s->type == TY_STRUCT) strcpy(g->local_type[g->nlocals], s->struct_name);
                else g->local_type[g->nlocals][0] = '\0';
                g->nlocals += asz;
            }
            break;
        case STMT_IF:
            collect_locals_stmt(g, s->thenb);
            collect_locals_stmt(g, s->elseb);
            break;
        case STMT_WHILE:
            collect_locals_stmt(g, s->body);
            break;
        case STMT_BLOCK:
            for (int i = 0; i < s->nstmts; i++) collect_locals_stmt(g, s->stmts[i]);
            break;
    }
}

/* collect from function params too */
static void collect_locals_func(struct codegen *g, Func *f) {
    g->nlocals = 0;
    /* Clear type metadata left by the previous function: slot indices are
       reused, and a stale array flag on a scalar parameter changes codegen. */
    for (int i = 0; i < 64; i++) {
        g->local_nelem[i] = 1;
        g->local_is_array[i] = 0;
        g->local_type[i][0] = '\0';
    }
    for (int i = 0; i < f->nparams; i++) {
        strcpy(g->locals[g->nlocals], f->params[i]);
        g->nlocals++;
    }
    collect_locals_stmt(g, f->body);
}

static void assign_offsets(struct codegen *g) {
    for (int i = 0; i < g->nlocals; i++)
        g->local_off[i] = i * 4;
    int ntemp = 8;
    g->spill_off = (g->nlocals + ntemp) * 4;
    g->saved_lr_off = (g->nlocals + ntemp + 1) * 4;
    g->frame_size = (g->nlocals + ntemp + 2) * 4;
}

static int new_label(struct codegen *g) { return g->label_counter++; }

/* forward decls */
static void gen_expr(struct codegen *g, Expr *e);
static void gen_stmt(struct codegen *g, Stmt *s);

static void emit_load_imm(struct codegen *g, const char *rd, long val) {
    if (val >= -32768 && val <= 32767) {
        cg_printf(g, "  ADDI %s, zero, %ld", rd, val);
    } else {
        /* LUI shifts by 11 */
        long hi = (val + 0x400) >> 11;
        long lo = val - (hi << 11);
        cg_printf(g, "  LUI %s, %ld", rd, hi);
        if (lo != 0) cg_printf(g, "  ADDI %s, %s, %ld", rd, rd, lo);
    }
}

static const char *expr_struct_type(struct codegen *g, Expr *e) {
    if (!e) return "";
    if (e->kind == EXPR_VAR) {
        int idx = find_local_idx(g, e->name);
        if (idx >= 0) return g->local_type[idx];
        int gi = find_global(g, e->name);
        if (gi >= 0) return g->globals[gi].type;
    }
    if (e->kind == EXPR_FIELD) {
        /* field of a struct: need the struct def of base */
        const char *sn = expr_struct_type(g, e->left);
        for (int i = 0; i < g->nstructs; i++)
            if (strcmp(g->structs[i].name, sn) == 0)
                return ""; /* fields are int in v0 */
    }
    return "";
}

static int field_index(struct codegen *g, const char *structname, const char *fieldname) {
    for (int i = 0; i < g->nstructs; i++) {
        if (strcmp(g->structs[i].name, structname) == 0) {
            for (int j = 0; j < g->structs[i].nfields; j++)
                if (strcmp(g->structs[i].fields[j], fieldname) == 0) return j;
        }
    }
    return -1;
}

static void gen_addr(struct codegen *g, Expr *e) {
    if (!e) { cg_emit(g, "  ADDI t0, zero, 0"); return; }
    if (e->kind == EXPR_VAR) {
        int off = find_local(g, e->name);
        if (off >= 0) {
            cg_printf(g, "  ADDI t0, sp, %d", off);
        } else {
            int gi = find_global(g, e->name);
            if (gi >= 0) emit_load_imm(g, "t0", g->gaddr[gi]);
            else cg_emit(g, "  ADDI t0, zero, 0");
        }
    } else if (e->kind == EXPR_DEREF) {
        gen_expr(g, e->left);
    } else if (e->kind == EXPR_INDEX) {
        gen_addr(g, e->left);
        cg_emit(g, "  ADD t1, t0, zero");
        gen_expr(g, e->right);
        cg_emit(g, "  SLLI t0, t0, 2");
        cg_emit(g, "  ADD t0, t1, t0");
    } else if (e->kind == EXPR_ADDR) {
        gen_addr(g, e->left);
    } else if (e->kind == EXPR_FIELD) {
        gen_addr(g, e->left);
        const char *sn = expr_struct_type(g, e->left);
        int fi = field_index(g, sn, e->name);
        if (fi >= 0) cg_printf(g, "  ADDI t0, t0, %d", fi * 4);
    } else {
        gen_expr(g, e);
    }
}

static void gen_expr(struct codegen *g, Expr *e) {
    if (!e) return;
    switch (e->kind) {
        case EXPR_INT:
            emit_load_imm(g, "t0", e->ival);
            break;
        case EXPR_VAR: {
            int off = find_local(g, e->name);
            if (off >= 0) {
                int idx = find_local_idx(g, e->name);
                int isarr = (idx >= 0) ? g->local_is_array[idx] : 0;
                if (isarr) {
                    cg_printf(g, "  ADDI t0, sp, %d", off);
                } else {
                    cg_printf(g, "  LDW t0, [sp + %d]", off);
                }
            } else {
                int gi = find_global(g, e->name);
                if (gi >= 0) {
                    if (g->globals[gi].nelem > 1) {
                        emit_load_imm(g, "t0", g->gaddr[gi]);
                    } else {
                        emit_load_imm(g, "t1", g->gaddr[gi]);
                        cg_emit(g, "  LDW t0, [t1 + 0]");
                    }
                } else {
                    cg_emit(g, "  ADDI t0, zero, 0");
                }
            }
            break;
        }
        case EXPR_UNOP:
            gen_expr(g, e->left);
            if (e->op == T_MINUS) {
                cg_emit(g, "  SUB t0, zero, t0");
            } else if (e->op == T_BANG) {
                cg_emit(g, "  SLTIU t0, t0, 1");
            }
            break;
        case EXPR_BINOP: {
            /* evaluate left -> t0, save to t5; evaluate right -> t0; op using t5,t0 */
            gen_expr(g, e->left);
            cg_emit(g, "  ADD t5, t0, zero");
            gen_expr(g, e->right);
            /* now t5 = left, t0 = right */
            int op = e->op;
            if (op == T_PLUS) {
                cg_emit(g, "  ADD t0, t5, t0");
            } else if (op == T_MINUS) {
                cg_emit(g, "  SUB t0, t5, t0");
            } else if (op == T_STAR) {
                cg_emit(g, "  MUL t0, t5, t0");
            } else if (op == T_SLASH) {
                cg_emit(g, "  DIV t0, t5, t0");
            } else if (op == T_PERCENT) {
                cg_emit(g, "  REM t0, t5, t0");
            } else if (op == T_AMP) {
                cg_emit(g, "  AND t0, t5, t0");
            } else if (op == T_PIPE) {
                cg_emit(g, "  OR t0, t5, t0");
            } else if (op == T_CARET) {
                cg_emit(g, "  XOR t0, t5, t0");
            } else if (op == T_LSHIFT) {
                cg_emit(g, "  SHL t0, t5, t0");
            } else if (op == T_RSHIFT) {
                cg_emit(g, "  SHR t0, t5, t0");
            } else if (op == T_EQ) {
                cg_emit(g, "  SUB t1, t5, t0");
                cg_emit(g, "  SLTIU t0, t1, 1");
            } else if (op == T_NE) {
                cg_emit(g, "  SUB t1, t5, t0");
                cg_emit(g, "  SLTIU t0, t1, 1");
                cg_emit(g, "  XORI t0, t0, 1");
            } else if (op == T_LT) {
                cg_emit(g, "  SUB t1, t5, t0");
                cg_emit(g, "  SRAI t0, t1, 31");
                cg_emit(g, "  ANDI t0, t0, 1");
            } else if (op == T_LE) {
                cg_emit(g, "  SUB t1, t0, t5");
                cg_emit(g, "  SRAI t0, t1, 31");
                cg_emit(g, "  ANDI t0, t0, 1");
                cg_emit(g, "  XORI t0, t0, 1");
            } else if (op == T_GT) {
                cg_emit(g, "  SUB t1, t0, t5");
                cg_emit(g, "  SRAI t0, t1, 31");
                cg_emit(g, "  ANDI t0, t0, 1");
            } else if (op == T_GE) {
                cg_emit(g, "  SUB t1, t5, t0");
                cg_emit(g, "  SRAI t0, t1, 31");
                cg_emit(g, "  ANDI t0, t0, 1");
                cg_emit(g, "  XORI t0, t0, 1");
            }
            break;
        }
        case EXPR_CALL: {
            /* evaluate args into temp slots, then load into a0..a7 */
            for (int i = 0; i < e->nargs; i++) {
                gen_expr(g, e->args[i]);
                cg_printf(g, "  STW t0, [sp + %d]", g->nlocals * 4 + i * 4);
            }
            for (int i = 0; i < e->nargs; i++) {
                cg_printf(g, "  LDW a%d, [sp + %d]", i, g->nlocals * 4 + i * 4);
            }
            cg_printf(g, "  CALL %s", e->name);
            cg_emit(g, "  ADDI t0, a0, 0");
            break;
        }
        case EXPR_ADDR: {
            if (e->left && e->left->kind == EXPR_VAR) {
                int off = find_local(g, e->left->name);
                if (off >= 0) {
                    cg_printf(g, "  ADDI t0, sp, %d", off);
                } else {
                    int gi = find_global(g, e->left->name);
                    if (gi >= 0) emit_load_imm(g, "t0", g->gaddr[gi]);
                    else cg_emit(g, "  ADDI t0, zero, 0");
                }
            } else if (e->left && (e->left->kind == EXPR_FIELD || e->left->kind == EXPR_INDEX)) {
                gen_addr(g, e->left);
            } else {
                gen_expr(g, e->left);
            }
            break;
        }
        case EXPR_INDEX: {
            gen_addr(g, e);
            cg_emit(g, "  LDW t0, [t0 + 0]");
            break;
        }
        case EXPR_FIELD:
        case EXPR_DEREF: {
            gen_addr(g, e);
            cg_emit(g, "  LDW t0, [t0 + 0]");
            break;
        }
    }
}
static void gen_stmt(struct codegen *g, Stmt *s) {
    if (!s) return;
    switch (s->kind) {
        case STMT_DECL: {
            int off = find_local(g, s->name);
            int asz = s->array_size ? s->array_size : 1;
            if (s->init) {
                gen_expr(g, s->init);
                cg_printf(g, "  STW t0, [sp + %d]", off);
            } else {
                cg_emit(g, "  ADDI t0, zero, 0");
                for (int j = 0; j < asz; j++)
                    cg_printf(g, "  STW t0, [sp + %d]", off + j * 4);
            }
            break;
        }
        case STMT_ASSIGN: {
            /* Evaluate the address first, spill it, then evaluate the value.
               The value expression may contain a call that clobbers temporaries. */
            gen_addr(g, s->target);
            cg_printf(g, "  STW t0, [sp + %d]", g->spill_off);
            gen_expr(g, s->expr);
            cg_printf(g, "  LDW t1, [sp + %d]", g->spill_off);
            cg_emit(g, "  STW t0, [t1 + 0]");
            break;
        }
        case STMT_IF: {
            gen_expr(g, s->cond);
            int lelse = new_label(g);
            int lend = new_label(g);
            cg_printf(g, "  BZ t0, L%d", lelse);
            gen_stmt(g, s->thenb);
            cg_printf(g, "  JMP L%d", lend);
            cg_printf(g, "L%d:", lelse);
            if (s->elseb) gen_stmt(g, s->elseb);
            cg_printf(g, "L%d:", lend);
            break;
        }
        case STMT_WHILE: {
            int lstart = new_label(g);
            int lend = new_label(g);
            cg_printf(g, "L%d:", lstart);
            gen_expr(g, s->cond);
            cg_printf(g, "  BZ t0, L%d", lend);
            gen_stmt(g, s->body);
            cg_printf(g, "  JMP L%d", lstart);
            cg_printf(g, "L%d:", lend);
            break;
        }
        case STMT_RETURN: {
            if (s->expr) {
                gen_expr(g, s->expr);
                cg_emit(g, "  ADDI a0, t0, 0");
            }
            /* epilogue */
            int nl = g->nlocals;
            (void)nl;
            cg_printf(g, "  LDW lr, [sp + %d]", g->saved_lr_off);
            cg_printf(g, "  ADDI sp, sp, %d", g->frame_size);
            cg_emit(g, "  RET");
            break;
        }
        case STMT_EXPR:
            gen_expr(g, s->expr);
            break;
        case STMT_BLOCK:
            for (int i = 0; i < s->nstmts; i++) gen_stmt(g, s->stmts[i]);
            break;
    }
}

static int stmt_ends_with_return(Stmt *s) {
    if (!s) return 0;
    switch (s->kind) {
        case STMT_RETURN: return 1;
        case STMT_BLOCK:
            if (s->nstmts > 0) return stmt_ends_with_return(s->stmts[s->nstmts-1]);
            return 0;
        case STMT_IF:
            return stmt_ends_with_return(s->thenb) && stmt_ends_with_return(s->elseb);
        default: return 0;
    }
}
static void gen_func(struct codegen *g, Func *f) {
    g->label_counter = 0;
    collect_locals_func(g, f);
    assign_offsets(g);
    /* header */
    cg_printf(g, "%s:", f->name);
    cg_printf(g, "  ADDI sp, sp, -%d", g->frame_size);
    int nl = g->nlocals;
    (void)nl;
    /* save lr */
    cg_printf(g, "  STW lr, [sp + %d]", g->saved_lr_off);
    /* store params into locals */
    for (int i = 0; i < f->nparams; i++) {
        /* find offset for param name */
        int off = -1;
        for (int j = 0; j < g->nlocals; j++)
            if (strcmp(g->locals[j], f->params[i]) == 0) { off = g->local_off[j]; break; }
        cg_printf(g, "  STW a%d, [sp + %d]", i, off);
    }
    gen_stmt(g, f->body);
    if (!stmt_ends_with_return(f->body)) {
        cg_printf(g, "  LDW lr, [sp + %d]", g->saved_lr_off);
        cg_printf(g, "  ADDI sp, sp, %d", g->frame_size);
        cg_emit(g, "  RET");
    }
}

static char *codegen(Program *prog) {
    struct codegen g;
    memset(&g, 0, sizeof g);
    g.buf = malloc(4096);
    g.buf[0] = '\0';
    g.cap = 4096;
    g.len = 0;
    /* globals */
    g.globals = prog->globals;
    g.nglobals = prog->nglobals;
    g.structs = prog->structs;
    g.nstructs = prog->nstructs;
    g.gaddr = malloc((prog->nglobals ? prog->nglobals : 1) * sizeof(int));
    int gbase = 0x1000;
    for (int i = 0; i < prog->nglobals; i++) {
        g.gaddr[i] = gbase;
        gbase += (prog->globals[i].nelem > 0 ? prog->globals[i].nelem : 1) * 4;
    }
    for (int i = 0; i < prog->nfuncs; i++) {
        gen_func(&g, prog->funcs[i]);
        cg_emit(&g, "");
    }
    /* data section */
    if (prog->nglobals > 0) {
        cg_printf(&g, ".org 0x1000");
        for (int i = 0; i < prog->nglobals; i++) {
            cg_printf(&g, "%s:", prog->globals[i].name);
            int ne = prog->globals[i].nelem > 0 ? prog->globals[i].nelem : 1;
            for (int j = 0; j < ne; j++) {
                if (j == 0 && prog->globals[i].has_init)
                    cg_printf(&g, "  .word %ld", prog->globals[i].init);
                else
                    cg_emit(&g, "  .word 0");
            }
        }
    }
    return g.buf;
}

/* =====================================================================
 *Public API
 * ===================================================================== */

int bradcmini_compile(const char *src, struct brad_as_output *out,
                      char *err, size_t errsz) {
    if (err && errsz) err[0] = '\0';
    struct parser p;
    memset(&p, 0, sizeof p);
    p.err = err;
    p.errsz = errsz;
    lex_init(&p.lx, src);
    p_advance(&p);
    Program *prog = parse_program(&p);
    if (!prog || (err && err[0])) {
        return -1;
    }
    char *asmtext = codegen(prog);
    int r = brad_as_assemble(asmtext, out);
    if (r != 0) {
        if (err && errsz) {
            snprintf(err, errsz, "assembly failed: %s", out->error.msg);
        }
        free(asmtext);
        return -1;
    }
    free(asmtext);
    return 0;
}

char *bradcmini_to_asm(const char *src, char *err, size_t errsz) {
    if (err && errsz) err[0] = '\0';
    struct parser p;
    memset(&p, 0, sizeof p);
    p.err = err;
    p.errsz = errsz;
    lex_init(&p.lx, src);
    p_advance(&p);
    Program *prog = parse_program(&p);
    if (!prog || (err && err[0])) {
        return NULL;
    }
    return codegen(prog);
}
