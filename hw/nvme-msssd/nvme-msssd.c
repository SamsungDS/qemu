/*
 * Memory Solution Lab, SAMSUNG Electronics
 * 2021-2022 Tong Zhang <t.zhang2@samsung.com>
 *
 * An Emulator for SAMSUNG Memory Semantic SSD
 *
 * SAMSUNG Memory Semantic SSD is a CXL Type 3 device that creats 1:1 mapping to
 * full LBA range of the SSD. This emulator is built on top of existing NVME
 * device and CXL Type 3 device emulator. The actual implementation is tricky
 * and have to modify NvmeCtrl definition since we are creating a new class and
 * having two base classes.
 * NVME Configuration options and CXL Type 3 device options applies here.
 */
#include "qemu/osdep.h"
#include "hw/cxl/cxl.h"
#include "hw/mem/memory-device.h"
#include "hw/nvme/nvme.h"
#include "qom/object.h"

#define TYPE_NVME_MSSSD "nvme-msssd"

// since TypeImpl is not exported (defined in qom/object.c)
// we add TypeImpl here to make it possible to call class_init and instance_init
struct TypeImpl
{
    void* opaque_0;
    size_t opaque[3];
    void (*class_init)(ObjectClass *klass, void *data);
    void *opaque_1;
    void *opaque_2;
    void (*instance_init)(Object *obj);
};

static void (*ct3d_realize)(PCIDevice *, Error **);
static void (*nvme_realize)(PCIDevice *, Error **);

static void nvme_msssd_realize(PCIDevice *pci_dev, Error **errp) {
    ct3d_realize(pci_dev, errp);
    nvme_realize(pci_dev, errp);
}

static void nvme_msssd_class_init(ObjectClass *oc, void *data)
{
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    assert(pc->realize);
    ct3d_realize = pc->realize;
    object_class_by_name(TYPE_NVME)->type->class_init(oc,data);
    nvme_realize = pc->realize;
    pc->realize = nvme_msssd_realize;
}

static void nvme_msssd_instance_init(Object *obj)
{
    object_class_by_name(TYPE_NVME)->type->instance_init(obj);
}

static const TypeInfo nvme_msssd_info = {
    .name          = TYPE_NVME_MSSSD,
    .parent        = TYPE_CXL_TYPE3,
    .instance_size = sizeof(NvmeCtrl),
    .instance_init = nvme_msssd_instance_init,
    .class_init    = nvme_msssd_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_MEMORY_DEVICE },
        { INTERFACE_CXL_DEVICE },
        { INTERFACE_PCIE_DEVICE },
        { }
    },
};

static void nvme_msssd_register_types(void)
{
    type_register_static(&nvme_msssd_info);
}

type_init(nvme_msssd_register_types)
