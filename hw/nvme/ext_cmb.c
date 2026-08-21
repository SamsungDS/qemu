#include "qemu/osdep.h"
#include "qemu/units.h"
#include "nvme.h"
#include "ext_cmb.h"
#include "trace.h"

#define NVME_CMB_BIR 2

bool nvme_addr_is_cmb(NvmeCtrl *n, hwaddr addr)
{
    hwaddr hi, lo;

    if (!n->state.cmb.cmse) {
        return false;
    }

    lo = n->params.cmb.legacy_mode ? n->state.cmb.mem.addr : n->state.cmb.cba;
    hi = lo + int128_get64(n->state.cmb.mem.size);

    return addr >= lo && addr < hi;
}

uint16_t nvme_map_addr_cmb(NvmeCtrl *n, QEMUIOVector *iov, hwaddr addr,
                                  size_t len)
{
    if (!len) {
        return NVME_SUCCESS;
    }

    trace_pci_nvme_map_addr_cmb(addr, len);

    if (!nvme_addr_is_cmb(n, addr) || !nvme_addr_is_cmb(n, addr + len - 1)) {
        return NVME_DATA_TRAS_ERROR;
    }

    qemu_iovec_add(iov, nvme_addr_to_cmb(&n->state.cmb, addr), len);

    return NVME_SUCCESS;
}

static void nvme_cmb_write(void *opaque, hwaddr addr, uint64_t data,
                           unsigned size)
{
    NvmeCtrl *n = (NvmeCtrl *)opaque;
    stn_le_p(&n->state.cmb.buf[addr], size, data);
}

static uint64_t nvme_cmb_read(void *opaque, hwaddr addr, unsigned size)
{
    NvmeCtrl *n = (NvmeCtrl *)opaque;
    return ldn_le_p(&n->state.cmb.buf[addr], size);
}

static const MemoryRegionOps nvme_cmb_ops = {
    .read = nvme_cmb_read,
    .write = nvme_cmb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

void nvme_cmb_enable_regs(NvmeCtrl *n)
{
    uint32_t cmbloc = ldl_le_p(&n->bar.cmbloc);
    uint32_t cmbsz = ldl_le_p(&n->bar.cmbsz);

    NVME_CMBLOC_SET_CDPCILS(cmbloc, 1);
    NVME_CMBLOC_SET_CDPMLS(cmbloc, 1);
    NVME_CMBLOC_SET_BIR(cmbloc, NVME_CMB_BIR);
    stl_le_p(&n->bar.cmbloc, cmbloc);

    NVME_CMBSZ_SET_SQS(cmbsz, 1);
    NVME_CMBSZ_SET_CQS(cmbsz, 0);
    NVME_CMBSZ_SET_LISTS(cmbsz, 1);
    NVME_CMBSZ_SET_RDS(cmbsz, 1);
    NVME_CMBSZ_SET_WDS(cmbsz, 1);
    NVME_CMBSZ_SET_SZU(cmbsz, 2); /* MBs */
    NVME_CMBSZ_SET_SZ(cmbsz, n->params.cmb.size_mb);
    stl_le_p(&n->bar.cmbsz, cmbsz);
}

static void nvme_init_cmb(NvmeCtrl *n, PCIDevice *pci_dev)
{
    uint64_t cmb_size = n->params.cmb.size_mb * MiB;
    uint64_t cap = ldq_le_p(&n->bar.cap);

    n->state.cmb.buf = g_malloc0(cmb_size);
    memory_region_init_io(&n->state.cmb.mem, OBJECT(n), &nvme_cmb_ops, n,
                          "nvme-cmb", cmb_size);
    pci_register_bar(pci_dev, NVME_CMB_BIR,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &n->state.cmb.mem);

    NVME_CAP_SET_CMBS(cap, 1);
    stq_le_p(&n->bar.cap, cap);

    if (n->params.cmb.legacy_mode) {
        nvme_cmb_enable_regs(n);
        n->state.cmb.cmse = true;
    }
}

bool nvme_cmb_on_init_state(NvmeCtrl *n, void *_data)
{
    NvmeExtCmbState *s = &n->state.cmb;
    NvmeExtCmbParams *p = n->exts.params[NVME_EXT_CMB];

    s->conf.legacy_mode = p->legacy_mode;
    s->conf.size_mb = p->size_mb;

    return true;
}

bool nvme_cmb_on_init_pci(NvmeCtrl *n, void *_data)
{
    nvme_init_cmb(n, PCI_DEVICE(n));
    return true;
}

bool nvme_cmb_on_exit(NvmeCtrl *n, void *_data)
{
    g_free(n->state.cmb.buf);
    return true;
}

