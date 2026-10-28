// FarNitroView for Far Manager: a parallel loop on the decoders' thread pool.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*par_fn)(size_t i, void *ctx);
// fn(0, ctx) ... fn(n - 1, ctx) on all cores; returns when all are done.
void par_for(size_t n, par_fn fn, void *ctx);
int par_ncpu(void);

#ifdef __cplusplus
}

// par_for with a lambda (called only while par_for runs)
template <class F> static inline void ParallelFor(size_t n, const F &f) {
    par_for(n, [](size_t i, void *c) { (*static_cast<const F *>(c))(i); }, (void *)&f);
}
#endif
