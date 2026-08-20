#ifndef HW_NVME_PMR_H
#define HW_NVME_PMR_H
#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "system/hostmem.h"

typedef struct NvmeCtrl NvmeCtrl;
typedef struct QEMUIOVector QEMUIOVector;

typedef struct NvmeExtPmrState {
    HostMemoryBackend *dev;
    bool              cmse;
    hwaddr            cba;
} NvmeExtPmrState;

typedef struct NvmeExtPmrParams {

} NvmeExtPmrParams;

bool nvme_addr_is_pmr(NvmeCtrl *n, hwaddr addr);
uint16_t nvme_map_addr_pmr(NvmeCtrl *n, QEMUIOVector *iov, hwaddr addr,
                                  size_t len);

bool nvme_pmr_on_check_params(NvmeCtrl *n, void *data);
bool nvme_pmr_on_init_state(NvmeCtrl *n, void *_data);
bool nvme_pmr_on_init_pci(NvmeCtrl *n, void *data);
bool nvme_pmr_on_ctrl_shutdown(NvmeCtrl *n, void *_data);

static inline void *nvme_addr_to_pmr(NvmeExtPmrState *s, hwaddr addr)
{
    return memory_region_get_ram_ptr(&s->dev->mr) + (addr - s->cba);
}

#endif /* HW_NVME_PMR_H */
