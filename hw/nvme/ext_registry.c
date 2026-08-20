#include "ext.h"
#include "ext_registry.h"
#include "ext_cmb.h"
#include "ext_pmr.h"

static const NvmeExtHook nvme_hooks_check_params[] = {
    { nvme_pmr_on_check_params, NVME_EXT_PMR },
};

static const NvmeExtHook nvme_hooks_init_state[] = {
    { nvme_cmb_on_init_state, NVME_EXT_CMB },
};

static const NvmeExtHook nvme_hooks_init_pci[] = {
    { nvme_cmb_on_init_pci, NVME_EXT_CMB },
    { nvme_pmr_on_init_pci, NVME_EXT_PMR },
};

static const NvmeExtHook nvme_hooks_ctrl_shutdown[] = {
    { nvme_pmr_on_ctrl_shutdown, NVME_EXT_PMR },
};

static const NvmeExtHook nvme_hooks_exit[] = {
    { nvme_cmb_on_exit, NVME_EXT_CMB },
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
