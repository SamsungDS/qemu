#include "ext.h"
#include "ext_registry.h"

static const NvmeExtHook nvme_hooks_check_params[] = {
};

static const NvmeExtHook nvme_hooks_init_state[] = {
};

static const NvmeExtHook nvme_hooks_init_pci[] = {
};

static const NvmeExtHook nvme_hooks_ctrl_shutdown[] = {
};

static const NvmeExtHook nvme_hooks_exit[] = {
};

#define PHASE_HOOKS(hooks) \
    { hooks, ARRAY_SIZE(hooks) }

const NvmeExtPhase
    nvme_ext_phases[NVME_EEV_COUNT] = {
    [NVME_EEV_CHECK_PARAMS] = PHASE_HOOKS(nvme_hooks_check_params),
    [NVME_EEV_INIT_STATE] = PHASE_HOOKS(nvme_hooks_init_state),
    [NVME_EEV_INIT_PCI] = PHASE_HOOKS(nvme_hooks_init_pci),
    [NVME_EEV_CTRL_SHUTDOWN] = PHASE_HOOKS(nvme_hooks_ctrl_shutdown),
    [NVME_EEV_EXIT] = PHASE_HOOKS(nvme_hooks_exit),
};
