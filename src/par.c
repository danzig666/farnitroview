// FarNitroView for Far Manager: the decoders' thread pool (nitro_os.h) for the plugin's own loops.
#include "par.h"
#include "nitro_os.h"

void par_for(size_t n, par_fn fn, void *ctx) {
    nitro_parallel(n, ^(size_t i) { fn(i, ctx); });
}

int par_ncpu(void) { return nitro_ncpu(); }
