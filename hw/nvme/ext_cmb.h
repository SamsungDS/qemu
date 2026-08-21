#ifndef HW_NVME_CMB_H
#define HW_NVME_CMB_H
#include "qemu/osdep.h"
#include "system/hostmem.h"
#include "exec/hwaddr.h"
#include "qemu/iov.h"
#include "hw/core/qdev.h"

typedef struct NvmeCtrl NvmeCtrl;

typedef struct NvmeExtCmbState {
    MemoryRegion mem;
    uint8_t      *buf;
    bool         cmse;
    hwaddr       cba;
    struct {
        uint32_t size_mb;
        bool legacy_mode;
    } conf;
} NvmeExtCmbState;

typedef struct NvmeExtCmbParams {
    uint32_t size_mb;
    bool legacy_mode;
} NvmeExtCmbParams;

bool nvme_addr_is_cmb(NvmeCtrl *n, hwaddr addr);
uint16_t nvme_map_addr_cmb(NvmeCtrl *n, QEMUIOVector *iov, hwaddr addr,
                                  size_t len);

bool nvme_cmb_on_init_state(NvmeCtrl *n, void *_data);
bool nvme_cmb_on_init_pci(NvmeCtrl *n, void *_data);
bool nvme_cmb_on_exit(NvmeCtrl *n, void *_data);

static inline void *nvme_addr_to_cmb(NvmeExtCmbState *s, hwaddr addr)
{
    hwaddr base = s->conf.legacy_mode ? s->mem.addr : s->cba;
    return &s->buf[addr - base];
}

void nvme_cmb_enable_regs(NvmeCtrl *n);
#endif /* HW_NVME_CMB_H */
