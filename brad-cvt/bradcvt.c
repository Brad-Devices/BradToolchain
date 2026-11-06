#include "brad/bradcvt.h"
#include "brad/bradvector.h"
#include "brad/bvbc.h"
#include "ptx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Last error for this thread.  The reference CLI and tests are the only
 * consumers today; keeping it thread-local means a future JIT can call the
 * translator from more than one host thread without a shared buffer. */
static _Thread_local char g_last_error[BRAD_CVT_MAX_LOG];

const char *brad_cvt_last_error(void)
{
    return g_last_error;
}

static void set_error(const struct cvt_diag *d, const char *fallback)
{
    if (d && d->msg[0]) {
        if (d->line > 0)
            snprintf(g_last_error, sizeof(g_last_error),
                     "line %d: %s", d->line, d->msg);
        else
            snprintf(g_last_error, sizeof(g_last_error), "%s", d->msg);
    } else if (fallback) {
        snprintf(g_last_error, sizeof(g_last_error), "%s", fallback);
    } else {
        g_last_error[0] = '\0';
    }
}

int brad_cvt_translate_ptx(const char *ptx_src, size_t ptx_len,
                           void **brad_vec_bin, size_t *bin_len)
{
    struct cvt_diag d;

    if (!brad_vec_bin || !bin_len) {
        set_error(NULL, "translate requires output pointers");
        return BRAD_CVT_ERR_PARSE;
    }
    *brad_vec_bin = NULL;
    *bin_len = 0;

    int st = cvt_ptx_translate(ptx_src, ptx_len, brad_vec_bin, bin_len,
                               NULL, &d);
    set_error(&d, NULL);

    if (st == BRAD_CVT_OK && (!*brad_vec_bin || *bin_len == 0)) {
        /* A translator that says OK and hands back nothing is the exact
         * failure this project keeps finding.  Treat it as an internal
         * error, not a success. */
        free(*brad_vec_bin);
        *brad_vec_bin = NULL;
        *bin_len = 0;
        set_error(NULL, "internal: translator returned no image");
        return BRAD_CVT_ERR_TRANSLATION;
    }
    return st;
}

int brad_cvt_translate_cubin(const void *cubin, size_t cubin_len,
                             void **brad_vec_bin, size_t *bin_len)
{
    (void)cubin;
    (void)cubin_len;
    if (brad_vec_bin)
        *brad_vec_bin = NULL;
    if (bin_len)
        *bin_len = 0;
    set_error(NULL,
              "cubin input is not implemented; Brad-CVT v0 consumes PTX text");
    return BRAD_CVT_ERR_UNSUPPORTED;
}

int brad_cvt_jit_invoke(const void *brad_vec_bin, size_t bin_len,
                        void *args, size_t args_size)
{
    (void)brad_vec_bin;
    (void)bin_len;
    (void)args;
    (void)args_size;
    set_error(NULL,
              "JIT invocation is not implemented; load the image with BVRT");
    return BRAD_CVT_ERR_UNSUPPORTED;
}

void brad_cvt_free(void *bin)
{
    free(bin);
}

const char *brad_cvt_status_str(int status)
{
    switch (status) {
    case BRAD_CVT_OK:              return "OK";
    case BRAD_CVT_ERR_PARSE:       return "parse error";
    case BRAD_CVT_ERR_TRANSLATION: return "translation error";
    case BRAD_CVT_ERR_UNSUPPORTED: return "unsupported";
    case BRAD_CVT_ERR_MEMORY:      return "out of memory";
    default:                       return "unknown status";
    }
}

/* ─── AOT driver: file in, file out ─── */

static int read_file(const char *path, char **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long sz;
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    sz = ftell(f);
    if (sz < 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return -1;
    }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    buf[sz] = '\0';
    fclose(f);
    *out = buf;
    *out_len = (size_t)sz;
    return 0;
}

static void dump_isa(const void *bin, size_t len)
{
    struct bvbc_image img;
    if (bvbc_unpack(bin, len, &img) != 0)
        return;
    printf("  kernel(s) = %u, instruction(s) = %u\n",
           img.nkernels, img.ninsn);
    for (uint32_t i = 0; i < img.ninsn; i++) {
        struct bv_insn d = bv_decode(img.insns[i]);
        if (d.op == BV_OP_CMP)
            printf("  %4u:  %-10s pred=%-2u dst=P%-3u src1=S%-3u "
                   "src2=S%-3u imm=%-4d flags=%u\n",
                   i, bv_opcode_name(d.op), d.pred, d.dst, d.src1, d.src2,
                   d.imm, d.flags);
        else
            printf("  %4u:  %-10s pred=%-2u dst=S%-3u src1=S%-3u "
                   "src2=S%-3u imm=%-4d flags=%u\n",
                   i, bv_opcode_name(d.op), d.pred, d.dst, d.src1, d.src2,
                   d.imm, d.flags);
    }
}

int brad_cvt_compile(struct brad_cvt_config *cfg,
                     struct brad_cvt_kernel_info *info)
{
    struct brad_cvt_kernel_info local;
    char *src = NULL;
    size_t src_len = 0;
    void *bin = NULL;
    size_t bin_len = 0;
    struct cvt_diag d;

    if (!info)
        info = &local;
    memset(info, 0, sizeof(*info));

    if (!cfg || !cfg->input_path) {
        set_error(NULL, "compile requires cfg->input_path");
        return BRAD_CVT_ERR_PARSE;
    }
    if (read_file(cfg->input_path, &src, &src_len) != 0) {
        set_error(NULL, "cannot read input file");
        return BRAD_CVT_ERR_PARSE;
    }

    int st = cvt_ptx_translate(src, src_len, &bin, &bin_len, info, &d);
    free(src);
    set_error(&d, NULL);

    if (st != BRAD_CVT_OK) {
        if (cfg->verbose)
            fprintf(stderr, "brad-cvt: %s\n",
                    g_last_error[0] ? g_last_error : brad_cvt_status_str(st));
        free(bin);
        return st;
    }
    if (!bin || bin_len == 0) {
        free(bin);
        set_error(NULL, "internal: compile produced no image");
        return BRAD_CVT_ERR_TRANSLATION;
    }

    if (cfg->output_path) {
        FILE *f = fopen(cfg->output_path, "wb");
        if (!f) {
            free(bin);
            set_error(NULL, "cannot open output file");
            return BRAD_CVT_ERR_PARSE;
        }
        if (fwrite(bin, 1, bin_len, f) != bin_len) {
            fclose(f);
            free(bin);
            set_error(NULL, "short write to output file");
            return BRAD_CVT_ERR_PARSE;
        }
        fclose(f);
    }

    if (cfg->dump_isa)
        dump_isa(bin, bin_len);

    free(bin);
    return BRAD_CVT_OK;
}
