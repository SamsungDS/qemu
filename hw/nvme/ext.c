#include "qemu/osdep.h"
#include "nvme.h"
#include "ext.h"
#include "ext_registry.h"

bool nvme_ext_enabled(NvmeExtRegistry *r, NvmeExtensionId id)
{
    if ((int)id >= NVME_EXT_COUNT) {
        return false;
    }
    return r->enabled[id];
}

void nvme_ext_enable(NvmeExtRegistry *r, NvmeExtensionId id, void *params)
{
    r->enabled[id] = true;
    r->params[id] = params;
}

bool nvme_ext_init(NvmeExtRegistry *r)
{
    int nhooks = 0;
    for (int i = 0; i < NVME_EEV_COUNT; i++) {
        NvmeExtPhase phase = nvme_ext_phases[i];
        for (int j = 0; j < phase.count; j++) {
            const NvmeExtHook *h = &phase.hooks[j];
            if (!r->enabled[h->ext]) {
                continue;
            }
            nhooks++;
        }
        nhooks++;
    }

    NvmeHookEntry *hes = g_new0(NvmeHookEntry, nhooks);
    int off = 0;

    for (int i = 0; i < NVME_EEV_COUNT; i ++) {
        NvmeExtPhase phase = nvme_ext_phases[i];
        r->phase_hooks_ndx[i] = off;
        for (int j = 0; j < phase.count; j++) {
            const NvmeExtHook *h = &phase.hooks[j];
            if (!r->enabled[h->ext]) {
                continue;
            }
            hes[off].fn = h->fn;
            off++;
        }
        off++;
    }

    r->hooks = hes;

    return true;
}

bool nvme_ext_call(NvmeCtrl *n, NvmeExtEvent phase, void *data)
{
    NvmeHookEntry *phase_hooks;

    if (unlikely((int)phase >= NVME_EEV_COUNT || !n->exts.hooks)) {
        return false;
    }

    phase_hooks = &n->exts.hooks[n->exts.phase_hooks_ndx[phase]];
    for (NvmeHookEntry *h = phase_hooks; h->fn; h++) {
        if (!h->fn(n, data)) {
            return false;
        }
    }

    return true;
}

void nvme_ext_free(NvmeExtRegistry *r)
{
    free(r->hooks);
    r->hooks = NULL;
}
