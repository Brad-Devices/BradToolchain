#include "brad/bradasm.h"
#include "bradcmini.h"
#include "brad_core_v2_emu.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void show(const char *src) {
    char err[256] = {0};
    char *asmtext = bradcmini_to_asm(src, err, sizeof err);
    if (!asmtext) { printf("COMPILE ERROR: %s\n", err); return; }
    printf("=== ASM ===\n%s\n", asmtext);
    free(asmtext);
}

/* Offset of the `main:` label in the emitted assembly, in bytes.  Assumes
   every non-label, non-directive line before it is one 32-bit instruction. */
static uint32_t find_entry(const char *asmtext) {
    const char *q = asmtext;
    uint32_t off = 0;
    char line[256];
    while (*q) {
        size_t i = 0;
        while (*q && *q != '\n' && i < sizeof(line) - 1) line[i++] = *q++;
        line[i] = '\0';
        if (*q == '\n') q++;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t L = strlen(s);
        while (L > 0 && (s[L-1] == ' ' || s[L-1] == '\t' || s[L-1] == '\r')) s[--L] = '\0';
        if (L == 0) continue;
        if (s[0] == ';') continue;
        if (s[L-1] == ':') {
            if (strcmp(s, "main:") == 0) return off;
            continue;
        }
        if (s[0] == '.') continue;
        off += 4;
    }
    return 0;
}

/* Compile, load at `main`, run to halt, return a0. */
static uint32_t compile_and_run(const char *src, uint64_t max_steps) {
    char err[256] = {0};
    char *asmtext = bradcmini_to_asm(src, err, sizeof err);
    assert(asmtext != NULL);
    uint32_t entry = find_entry(asmtext);
    free(asmtext);

    struct brad_as_output out;
    memset(&out, 0, sizeof out);
    assert(bradcmini_compile(src, &out, err, sizeof err) == 0);

    brad_v2_state st;
    brad_v2_init(&st);
    assert(brad_v2_load(&st, out.code, out.count, entry) == 0);
    brad_v2_run(&st, max_steps);
    uint32_t r = st.regs[BRAD_V2_A0];
    brad_v2_free(&st);
    return r;
}

static void test_add(void) {
    const char *src =
        "int add(int a, int b) {\n"
        "    return a + b;\n"
        "}\n";
    struct brad_as_output out;
    memset(&out, 0, sizeof out);
    char err[256] = {0};
    assert(bradcmini_compile(src, &out, err, sizeof err) == 0);
    printf("add: %d words assembled\n", out.count);
    /* sanity: contains RET */
    assert(out.count >= 2);
    assert((out.code[out.count-1] & 0xFC000000) == 0xB8000000); /* RET */
    show(src);
}

static void test_if_while(void) {
    const char *src =
        "int sum(int n) {\n"
        "    int s = 0;\n"
        "    int i = 0;\n"
        "    while (i < n) {\n"
        "        s = s + i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return s;\n"
        "}\n";
    struct brad_as_output out;
    memset(&out, 0, sizeof out);
    char err[256] = {0};
    assert(bradcmini_compile(src, &out, err, sizeof err) == 0);
    printf("sum: %d words assembled\n", out.count);
    assert(out.count >= 4);
    show(src);
}

static void test_call(void) {
    const char *src =
        "int add(int a, int b) { return a + b; }\n"
        "int main(void) { return add(3, 4); }\n";
    struct brad_as_output out;
    memset(&out, 0, sizeof out);
    char err[256] = {0};
    assert(bradcmini_compile(src, &out, err, sizeof err) == 0);
    printf("call: %d words assembled\n", out.count);
    show(src);
}

static void test_error(void) {
    const char *src = "int f(int a) { return a + ; }\n";
    struct brad_as_output out;
    memset(&out, 0, sizeof out);
    char err[256] = {0};
    assert(bradcmini_compile(src, &out, err, sizeof err) != 0);
    printf("error path: %s\n", err);
}

static void test_exec(void) {
    uint32_t r = compile_and_run(
        "int add(int a, int b) { return a + b; }\n"
        "int main(void) { return add(3, 4); }\n", 100000);
    printf("exec add(3,4): a0 = %u\n", r);
    assert(r == 7);

    r = compile_and_run(
        "int sum(int n) {\n"
        "    int s = 0;\n"
        "    int i = 0;\n"
        "    while (i < n) { s = s + i; i = i + 1; }\n"
        "    return s;\n"
        "}\n"
        "int main(void) { return sum(10); }\n", 1000000);
    printf("exec sum(10): a0 = %u\n", r);
    assert(r == 45);

    /* Function-local slot metadata must not leak between functions: slot 0 is
       an array in first() and a scalar parameter in second(). */
    r = compile_and_run(
        "int first(void) { int a[3]; a[0] = 7; a[1] = 8; a[2] = 9; return a[0]; }\n"
        "int second(int x) { return x; }\n"
        "int main(void) { return first() + second(5); }\n", 100000);
    printf("exec slot reuse: a0 = %u\n", r);
    assert(r == 12);
}

static void test_eight_params(void) {
    /* The published limit is eight arguments (a0-a7). The parser used to
       reject the eighth; this is the regression that pins the boundary. */
    uint32_t r = compile_and_run(
        "int sum8(int a, int b, int c, int d, int e, int f, int g, int h) {\n"
        "    return a + b + c + d + e + f + g + h;\n"
        "}\n"
        "int main(void) { return sum8(1, 2, 3, 4, 5, 6, 7, 8); }\n", 100000);
    printf("exec sum8(1..8): a0 = %u\n", r);
    assert(r == 36);
}

static void test_globals(void) {
    uint32_t r = compile_and_run(
        "int g = 5;\n"
        "int main(void) { g = g + 1; return g; }\n", 100000);
    printf("exec global g=g+1: a0 = %u\n", r);
    assert(r == 6);
}

static void test_arrays(void) {
    uint32_t r = compile_and_run(
        "int main(void) {\n"
        "    int arr[3];\n"
        "    arr[0] = 10; arr[1] = 20; arr[2] = 30;\n"
        "    return arr[0] + arr[1] + arr[2];\n"
        "}\n", 100000);
    printf("exec array sum: a0 = %u\n", r);
    assert(r == 60);
}

static void test_structs(void) {
    uint32_t r = compile_and_run(
        "struct Point { int x; int y; };\n"
        "int main(void) {\n"
        "    struct Point p;\n"
        "    p.x = 10; p.y = 20;\n"
        "    return p.x + p.y;\n"
        "}\n", 100000);
    printf("exec struct sum: a0 = %u\n", r);
    assert(r == 30);
}

static void test_pointers(void) {
    uint32_t r = compile_and_run(
        "int main(void) {\n"
        "    int x = 5;\n"
        "    int *p = &x;\n"
        "    *p = *p + 1;\n"
        "    return x;\n"
        "}\n", 100000);
    printf("exec pointer read/write: a0 = %u\n", r);
    assert(r == 6);
}

static void test_combined(void) {
    uint32_t r = compile_and_run(
        "struct Point { int x; int y; };\n"
        "int g = 5;\n"
        "int main(void) {\n"
        "    g = g + 1;\n"
        "    int arr[3];\n"
        "    arr[0] = 10; arr[1] = 20; arr[2] = 30;\n"
        "    int s = arr[0] + arr[1] + arr[2];\n"
        "    struct Point p;\n"
        "    p.x = 1; p.y = 2;\n"
        "    int *pp = &p.x;\n"
        "    *pp = 7;\n"
        "    return g + s + p.x + p.y;\n"
        "}\n", 1000000);
    printf("exec combined: a0 = %u (expected 75)\n", r);
    assert(r == 75);
}

int main(void) {
    test_add();
    test_if_while();
    test_call();
    test_error();
    test_exec();
    test_eight_params();
    test_globals();
    test_arrays();
    test_structs();
    test_pointers();
    test_combined();
    printf("bradc-mini: PASS\n");
    return 0;
}
