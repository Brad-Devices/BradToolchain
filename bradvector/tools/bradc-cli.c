/* bradc-cli — reference BradVector assembler front-end
 *
 *   bradc-cli kernel.bvbs -o kernel.bvbc
 *
 * Reads a BradVector assembly source (.bvbs) and writes a .bvbc
 * bytecode image.  A disassembly pass (`-d`) prints the stream.
 */

#include "brad/bradvector.h"
#include "brad/bvbc.h"
#include "bradc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s input.bvbs [-o output.bvbc] [-d]\n", prog);
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL;
    int disasm = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0) {
            disasm = 1;
        } else if (argv[i][0] != '-') {
            in = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!in) {
        usage(argv[0]);
        return 2;
    }
    if (!out)
        out = "a.bvbc";

    struct bradc_error err;
    memset(&err, 0, sizeof(err));

    uint64_t insn_buf[BVBC_MAX_INSN];
    struct bvbc_image img;
    memset(&img, 0, sizeof(img));
    img.insns = insn_buf;
    img.ninsn = 0;

    char *src = NULL;
    {
        FILE *f = fopen(in, "rb");
        if (!f) {
            fprintf(stderr, "bradc: cannot open %s\n", in);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < 0 || sz > 4 * 1024 * 1024) {
            fclose(f);
            fprintf(stderr, "bradc: bad source size\n");
            return 1;
        }
        src = malloc((size_t)sz + 1);
        if (!src) {
            fclose(f);
            fprintf(stderr, "bradc: out of memory\n");
            return 1;
        }
        if (fread(src, 1, (size_t)sz, f) != (size_t)sz) {
            fclose(f);
            fprintf(stderr, "bradc: read failed\n");
            free(src);
            return 1;
        }
        src[sz] = '\0';
        fclose(f);
    }

    if (bradc_assemble(src, &img, &err) < 0) {
        fprintf(stderr, "bradc: line %d: %s\n", err.line, err.msg);
        free(src);
        return 1;
    }
    free(src);

    size_t nbytes = bvbc_serialize_size(&img);
    uint8_t *buf = malloc(nbytes);
    if (!buf) {
        fprintf(stderr, "bradc: out of memory\n");
        return 1;
    }
    size_t packed = bvbc_pack(&img, buf, nbytes);
    if (packed == 0) {
        fprintf(stderr, "bradc: serialize failed\n");
        free(buf);
        return 1;
    }
    nbytes = packed;

    FILE *f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "bradc: cannot write %s\n", out);
        free(buf);
        return 1;
    }
    fwrite(buf, 1, nbytes, f);
    fclose(f);

    printf("bradc: assembled %d instruction(s), %zu byte(s) -> %s\n",
           img.ninsn, nbytes, out);

    if (disasm) {
        for (uint32_t i = 0; i < img.ninsn; i++) {
            struct bv_insn d = bv_decode(img.insns[i]);
            printf("%4u:  %s  pred=%u dst=%u src1=%u src2=%u imm=%d\n", i,
                   bv_opcode_name(d.op), d.pred, d.dst, d.src1, d.src2,
                   d.imm);
        }
    }

    free(buf);
    return 0;
}