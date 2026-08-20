#ifndef HW_NVME_EXT_H
#define HW_NVME_EXT_H
#include "qemu/osdep.h"

typedef enum NvmeExtensionId {
    NVME_EXT_NONE = 0,
    NVME_EXT_COUNT,
} NvmeExtensionId;

typedef enum NvmeExtEvent {
    NVME_EEV_CHECK_PARAMS,
    NVME_EEV_INIT_STATE,
    NVME_EEV_INIT_PCI,
    NVME_EEV_INIT_CTRL,
    NVME_EEV_INIT_NS,
    NVME_EEV_RESET_FUNC,
    NVME_EEV_CTRL_SHUTDOWN,
    NVME_EEV_NS_SHUTDOWN,
    NVME_EEV_EXIT,
    NVME_EEV_COUNT,
} NvmeExtEvent;

typedef struct NvmeCtrl NvmeCtrl;

typedef struct NvmeExtHook {
    bool (*fn)(NvmeCtrl *c, void *data);
    NvmeExtensionId ext;
} NvmeExtHook;

typedef struct NvmeExtPhase {
    const NvmeExtHook *hooks;
    int count;
} NvmeExtPhase;

typedef struct NvmeHookEntry {
    bool (*fn)(NvmeCtrl *c, void *data);
} NvmeHookEntry;

typedef struct NvmeExtRegistry {
    bool enabled[NVME_EXT_COUNT];
    void *params[NVME_EXT_COUNT];
    NvmeHookEntry *hooks;
    int phase_hooks_ndx[NVME_EEV_COUNT];
} NvmeExtRegistry;


bool nvme_ext_enabled(NvmeExtRegistry *r, NvmeExtensionId id);
void nvme_ext_enable(NvmeExtRegistry *r, NvmeExtensionId id, void *params);
bool nvme_ext_init(NvmeExtRegistry *r);
void nvme_ext_free(NvmeExtRegistry *r);
bool nvme_ext_call(NvmeCtrl *n, NvmeExtEvent phase, void *data);

#endif /* HW_NVME_EXT_H */
