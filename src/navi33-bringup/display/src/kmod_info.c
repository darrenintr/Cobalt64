// Standalone IOFramebuffer kext glue. Does not access GPU at load/unload.
#include <mach/mach_types.h>
#include <libkern/OSKextLib.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

static kern_return_t n33fb_start(kmod_info_t *ki, void *data) {
    (void)ki; (void)data;
    return KERN_SUCCESS;
}
static kern_return_t n33fb_stop(kmod_info_t *ki, void *data) {
    (void)ki; (void)data;
    return KERN_SUCCESS;
}

KMOD_EXPLICIT_DECL(com.cobalt64.navi33gopfb, "0.0.1", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = n33fb_start;
__private_extern__ kmod_stop_func_t *_antimain = n33fb_stop;
