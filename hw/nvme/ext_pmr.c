#include "qemu/osdep.h"
#include "nvme.h"
#include "ext_pmr.h"

#define NVME_PMR_BIR 4

bool nvme_addr_is_pmr(NvmeCtrl *n, hwaddr addr)
{
    hwaddr hi;

    if (!n->state.pmr.cmse) {
        return false;
    }

    hi = n->state.pmr.cba + int128_get64(n->state.pmr.dev->mr.size);

    return addr >= n->state.pmr.cba && addr < hi;
}


uint16_t nvme_map_addr_pmr(NvmeCtrl *n, QEMUIOVector *iov, hwaddr addr,
                           size_t len)
{
    if (!len) {
        return NVME_SUCCESS;
    }

    if (!nvme_addr_is_pmr(n, addr) || !nvme_addr_is_pmr(n, addr + len - 1)) {
        return NVME_DATA_TRAS_ERROR;
    }

    qemu_iovec_add(iov, nvme_addr_to_pmr(&n->state.pmr, addr), len);

    return NVME_SUCCESS;
}

static bool nvme_init_pmr(NvmeCtrl *n, PCIDevice *pci_dev, Error **errp)
{
    uint32_t pmrcap = ldl_le_p(&n->bar.pmrcap);

    if (memory_region_size(&n->state.pmr.dev->mr) < 16) {
        error_setg(errp, "PMR device must have at least 16 bytes");
        return false;
    }

    NVME_PMRCAP_SET_RDS(pmrcap, 1);
    NVME_PMRCAP_SET_WDS(pmrcap, 1);
    NVME_PMRCAP_SET_BIR(pmrcap, NVME_PMR_BIR);
    /* Turn on bit 1 support */
    NVME_PMRCAP_SET_PMRWBM(pmrcap, 0x02);
    NVME_PMRCAP_SET_CMSS(pmrcap, 1);
    stl_le_p(&n->bar.pmrcap, pmrcap);

    pci_register_bar(pci_dev, NVME_PMR_BIR,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &n->state.pmr.dev->mr);

    memory_region_set_enabled(&n->state.pmr.dev->mr, false);

    return true;
}

bool nvme_pmr_on_check_params(NvmeCtrl *n, void *data)
{
    NvmeParams *params = &n->params;
    Error **errp = data;

    if (params->msix_exclusive_bar) {
        error_setg(errp, "not enough BARs available to enable PMR");
        return false;
    }

    if (host_memory_backend_is_mapped(n->state.pmr.dev)) {
        error_setg(errp, "can't use already busy memdev: %s",
                   object_get_canonical_path_component(OBJECT(n->state.pmr.dev)));
        return false;
    }

    if (!is_power_of_2(n->state.pmr.dev->size)) {
        error_setg(errp, "pmr backend size needs to be power of 2 in size");
        return false;
    }

    return true;
}

bool nvme_pmr_on_init_state(NvmeCtrl *n, void *_data)
{
    host_memory_backend_set_mapped(n->state.pmr.dev, true);
    return true;
}

bool nvme_pmr_on_init_pci(NvmeCtrl *n, void *data)
{
    Error **errp = data;
    return nvme_init_pmr(n, PCI_DEVICE(n), errp);
}

bool nvme_pmr_on_ctrl_shutdown(NvmeCtrl *n, void *_data)
{
    memory_region_msync(&n->state.pmr.dev->mr, 0, n->state.pmr.dev->size);
    return true;
}

