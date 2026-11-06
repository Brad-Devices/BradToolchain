#ifndef BRAD_CVT_H
#define BRAD_CVT_H

#include <stdint.h>
#include <stddef.h>

#define BRAD_CVT_MAX_KERNEL_NAME  128
#define BRAD_CVT_MAX_LOG          4096

enum brad_cvt_mode {
    BRAD_CVT_AOT = 0,
    BRAD_CVT_JIT = 1,
};

enum brad_cvt_status {
    BRAD_CVT_OK               = 0,
    BRAD_CVT_ERR_PARSE        = -1,
    BRAD_CVT_ERR_TRANSLATION  = -2,
    BRAD_CVT_ERR_UNSUPPORTED  = -3,
    BRAD_CVT_ERR_MEMORY       = -4,
};

struct brad_cvt_kernel_info {
    char      name[BRAD_CVT_MAX_KERNEL_NAME];
    unsigned  num_regs;
    unsigned  shared_mem_bytes;
    unsigned  thread_count;
    unsigned  block_count;
    uint64_t  instruction_count;
};

struct brad_cvt_config {
    enum brad_cvt_mode    mode;
    const char           *input_path;
    const char           *output_path;
    const char           *arch;          
    int                   dump_isa;
    int                   optimize;
    int                   verbose;
    int                   num_threads;
};

int  brad_cvt_compile(struct brad_cvt_config *cfg,
                      struct brad_cvt_kernel_info *info);
int  brad_cvt_translate_ptx(const char *ptx_src,
                            size_t ptx_len,
                            void **brad_vec_bin,
                            size_t *bin_len);
int  brad_cvt_translate_cubin(const void *cubin,
                              size_t cubin_len,
                              void **brad_vec_bin,
                              size_t *bin_len);
void brad_cvt_free(void *bin);
const char *brad_cvt_status_str(int status);

/* Last diagnostic for the calling thread.  Never NULL; empty when the last
 * call succeeded.  This exists so a failed translation can say which line
 * and which construct, instead of returning a bare -3. */
const char *brad_cvt_last_error(void);

int  brad_cvt_jit_invoke(const void *brad_vec_bin,
                         size_t bin_len,
                         void *args,
                         size_t args_size);

#endif
