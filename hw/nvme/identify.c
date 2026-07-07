#include "qemu/osdep.h"
#include "nvme.h"
#include "identify.h"
#include "trace.h"

uint16_t nvme_rpt_empty_id_struct(NvmeCtrl *n, NvmeRequest *req)
{
    uint8_t id[NVME_IDENTIFY_DATA_SIZE] = {};

    return nvme_c2h(n, id, sizeof(id), req);
}

uint16_t nvme_identify_ctrl(NvmeCtrl *n, NvmeRequest *req, bool _)
{
    trace_pci_nvme_identify_ctrl();

    return nvme_c2h(n, (uint8_t *)&n->id_ctrl, sizeof(n->id_ctrl), req);
}

uint16_t nvme_identify_ctrl_csi(NvmeCtrl *n, NvmeRequest *req, bool _)
{
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    uint8_t id[NVME_IDENTIFY_DATA_SIZE] = {};
    NvmeIdCtrlNvm *id_nvm = (NvmeIdCtrlNvm *)&id;

    trace_pci_nvme_identify_ctrl_csi(c->csi);

    switch (c->csi) {
    case NVME_CSI_NVM:
        id_nvm->vsl = n->params.vsl;
        id_nvm->dmrl = NVME_ID_CTRL_NVM_DMRL_MAX;
        id_nvm->dmrsl = cpu_to_le32(n->dmrsl);
        id_nvm->dmsl = NVME_ID_CTRL_NVM_DMRL_MAX * n->dmrsl;
        break;

    case NVME_CSI_ZONED:
        ((NvmeIdCtrlZoned *)&id)->zasl = n->params.zasl;
        break;

    default:
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    return nvme_c2h(n, id, sizeof(id), req);
}

uint16_t nvme_identify_ns(NvmeCtrl *n, NvmeRequest *req, bool active)
{
    NvmeNamespace *ns;
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    uint32_t nsid = le32_to_cpu(c->nsid);

    trace_pci_nvme_identify_ns(nsid);

    if (!nvme_nsid_valid(n, nsid) || nsid == NVME_NSID_BROADCAST) {
        return NVME_INVALID_NSID | NVME_DNR;
    }

    ns = nvme_ns(n, nsid);
    if (unlikely(!ns)) {
        if (!active) {
            ns = nvme_subsys_ns(n->subsys, nsid);
            if (!ns) {
                return nvme_rpt_empty_id_struct(n, req);
            }
        } else {
            return nvme_rpt_empty_id_struct(n, req);
        }
    }

    if (active || ns->csi == NVME_CSI_NVM) {
        return nvme_c2h(n, (uint8_t *)&ns->id_ns, sizeof(NvmeIdNs), req);
    }

    return NVME_INVALID_IOCS | NVME_DNR;
}

uint16_t nvme_identify_ctrl_list(NvmeCtrl *n, NvmeRequest *req,
                                 bool attached)
{
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    uint32_t nsid = le32_to_cpu(c->nsid);
    uint16_t min_id = le16_to_cpu(c->ctrlid);
    uint16_t list[NVME_CONTROLLER_LIST_SIZE] = {};
    uint16_t *ids = &list[1];
    NvmeNamespace *ns;
    NvmeCtrl *ctrl;
    int cntlid, nr_ids = 0;

    trace_pci_nvme_identify_ctrl_list(c->cns, min_id);

    if (!n->subsys) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (attached) {
        if (nsid == NVME_NSID_BROADCAST) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }

        ns = nvme_subsys_ns(n->subsys, nsid);
        if (!ns) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }
    }

    for (cntlid = min_id; cntlid < ARRAY_SIZE(n->subsys->ctrls); cntlid++) {
        ctrl = nvme_subsys_ctrl(n->subsys, cntlid);
        if (!ctrl) {
            continue;
        }

        if (attached && !nvme_ns(ctrl, nsid)) {
            continue;
        }

        ids[nr_ids++] = cntlid;
    }

    list[0] = nr_ids;

    return nvme_c2h(n, (uint8_t *)list, sizeof(list), req);
}

uint16_t nvme_identify_pri_ctrl_cap(NvmeCtrl *n, NvmeRequest *req, bool _)
{
    trace_pci_nvme_identify_pri_ctrl_cap(le16_to_cpu(n->pri_ctrl_cap.cntlid));

    return nvme_c2h(n, (uint8_t *)&n->pri_ctrl_cap,
                    sizeof(NvmePriCtrlCap), req);
}

uint16_t nvme_identify_sec_ctrl_list(NvmeCtrl *n, NvmeRequest *req, bool _)
{
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    uint16_t pri_ctrl_id = le16_to_cpu(n->pri_ctrl_cap.cntlid);
    uint16_t min_id = le16_to_cpu(c->ctrlid);
    uint8_t num_sec_ctrl = n->nr_sec_ctrls;
    NvmeSecCtrlList list = {0};
    uint8_t i;

    for (i = 0; i < num_sec_ctrl; i++) {
        if (n->sec_ctrl_list[i].scid >= min_id) {
            list.numcntl = MIN(num_sec_ctrl - i, 127);
            memcpy(&list.sec, n->sec_ctrl_list + i,
                   list.numcntl * sizeof(NvmeSecCtrlEntry));
            break;
        }
    }

    trace_pci_nvme_identify_sec_ctrl_list(pri_ctrl_id, list.numcntl);

    return nvme_c2h(n, (uint8_t *)&list, sizeof(list), req);
}

uint16_t nvme_identify_ns_ind(NvmeCtrl *n, NvmeRequest *req, bool alloc)
{
    NvmeNamespace *ns;
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    uint32_t nsid = le32_to_cpu(c->nsid);

    trace_pci_nvme_identify_ns_ind(nsid);

    if (!nvme_nsid_valid(n, nsid) || nsid == NVME_NSID_BROADCAST) {
        return NVME_INVALID_NSID | NVME_DNR;
    }

    ns = nvme_ns(n, nsid);
    if (unlikely(!ns)) {
        if (alloc) {
            ns = nvme_subsys_ns(n->subsys, nsid);
            if (!ns) {
                return nvme_rpt_empty_id_struct(n, req);
            }
        } else {
            return nvme_rpt_empty_id_struct(n, req);
        }
    }

    return nvme_c2h(n, (uint8_t *)&ns->id_ns_ind, sizeof(NvmeIdNsInd), req);
}

uint16_t nvme_identify_ns_csi(NvmeCtrl *n, NvmeRequest *req,
                              bool active)
{
    NvmeNamespace *ns;
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    uint32_t nsid = le32_to_cpu(c->nsid);

    trace_pci_nvme_identify_ns_csi(nsid, c->csi);

    if (!nvme_nsid_valid(n, nsid) || nsid == NVME_NSID_BROADCAST) {
        return NVME_INVALID_NSID | NVME_DNR;
    }

    ns = nvme_ns(n, nsid);
    if (unlikely(!ns)) {
        if (!active) {
            ns = nvme_subsys_ns(n->subsys, nsid);
            if (!ns) {
                return nvme_rpt_empty_id_struct(n, req);
            }
        } else {
            return nvme_rpt_empty_id_struct(n, req);
        }
    }

    if (c->csi == NVME_CSI_NVM) {
        return nvme_c2h(n, (uint8_t *)&ns->id_ns_nvm, sizeof(NvmeIdNsNvm),
                        req);
    } else if (c->csi == NVME_CSI_ZONED && ns->csi == NVME_CSI_ZONED) {
        return nvme_c2h(n, (uint8_t *)ns->id_ns_zoned, sizeof(NvmeIdNsZoned),
                        req);
    }

    return NVME_INVALID_FIELD | NVME_DNR;
}

uint16_t nvme_identify_nslist(NvmeCtrl *n, NvmeRequest *req,
                              bool active)
{
    NvmeNamespace *ns;
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    uint32_t min_nsid = le32_to_cpu(c->nsid);
    uint8_t list[NVME_IDENTIFY_DATA_SIZE] = {};
    static const int data_len = sizeof(list);
    uint32_t *list_ptr = (uint32_t *)list;
    int i, j = 0;

    trace_pci_nvme_identify_nslist(min_nsid);

    /*
     * Both FFFFFFFFh (NVME_NSID_BROADCAST) and FFFFFFFFEh are invalid values
     * since the Active Namespace ID List should return namespaces with ids
     * *higher* than the NSID specified in the command. This is also specified
     * in the spec (NVM Express v1.3d, Section 5.15.4).
     */
    if (min_nsid >= NVME_NSID_BROADCAST - 1) {
        return NVME_INVALID_NSID | NVME_DNR;
    }

    for (i = 1; i <= NVME_MAX_NAMESPACES; i++) {
        ns = nvme_ns(n, i);
        if (!ns) {
            if (!active) {
                ns = nvme_subsys_ns(n->subsys, i);
                if (!ns) {
                    continue;
                }
            } else {
                continue;
            }
        }
        if (ns->params.nsid <= min_nsid) {
            continue;
        }
        list_ptr[j++] = cpu_to_le32(ns->params.nsid);
        if (j == data_len / sizeof(uint32_t)) {
            break;
        }
    }

    return nvme_c2h(n, list, data_len, req);
}

uint16_t nvme_identify_nslist_csi(NvmeCtrl *n, NvmeRequest *req,
                                  bool active)
{
    NvmeNamespace *ns;
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    uint32_t min_nsid = le32_to_cpu(c->nsid);
    uint8_t list[NVME_IDENTIFY_DATA_SIZE] = {};
    static const int data_len = sizeof(list);
    uint32_t *list_ptr = (uint32_t *)list;
    int i, j = 0;

    trace_pci_nvme_identify_nslist_csi(min_nsid, c->csi);

    /*
     * Same as in nvme_identify_nslist(), FFFFFFFFh/FFFFFFFFEh are invalid.
     */
    if (min_nsid >= NVME_NSID_BROADCAST - 1) {
        return NVME_INVALID_NSID | NVME_DNR;
    }

    if (c->csi != NVME_CSI_NVM && c->csi != NVME_CSI_ZONED) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    for (i = 1; i <= NVME_MAX_NAMESPACES; i++) {
        ns = nvme_ns(n, i);
        if (!ns) {
            if (!active) {
                ns = nvme_subsys_ns(n->subsys, i);
                if (!ns) {
                    continue;
                }
            } else {
                continue;
            }
        }
        if (ns->params.nsid <= min_nsid || c->csi != ns->csi) {
            continue;
        }
        list_ptr[j++] = cpu_to_le32(ns->params.nsid);
        if (j == data_len / sizeof(uint32_t)) {
            break;
        }
    }

    return nvme_c2h(n, list, data_len, req);
}

uint16_t nvme_endurance_group_list(NvmeCtrl *n, NvmeRequest *req, bool _)
{
    uint16_t list[NVME_CONTROLLER_LIST_SIZE] = {};
    uint16_t *nr_ids = &list[0];
    uint16_t *ids = &list[1];
    uint16_t endgid = le32_to_cpu(req->cmd.cdw11) & 0xffff;

    /*
     * The current nvme-subsys only supports Endurance Group #1.
     */
    if (!endgid) {
        *nr_ids = 1;
        ids[0] = 1;
    } else {
        *nr_ids = 0;
    }

    return nvme_c2h(n, list, sizeof(list), req);
}

uint16_t nvme_identify_ns_descr_list(NvmeCtrl *n, NvmeRequest *req, bool _)
{
    NvmeNamespace *ns;
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    uint32_t nsid = le32_to_cpu(c->nsid);
    uint8_t list[NVME_IDENTIFY_DATA_SIZE] = {};
    uint8_t *pos = list;
    struct {
        NvmeIdNsDescr hdr;
        uint8_t v[NVME_NIDL_UUID];
    } QEMU_PACKED uuid = {};
    struct {
        NvmeIdNsDescr hdr;
        uint8_t v[NVME_NIDL_NGUID];
    } QEMU_PACKED nguid = {};
    struct {
        NvmeIdNsDescr hdr;
        uint64_t v;
    } QEMU_PACKED eui64 = {};
    struct {
        NvmeIdNsDescr hdr;
        uint8_t v;
    } QEMU_PACKED csi = {};

    trace_pci_nvme_identify_ns_descr_list(nsid);

    if (!nvme_nsid_valid(n, nsid) || nsid == NVME_NSID_BROADCAST) {
        return NVME_INVALID_NSID | NVME_DNR;
    }

    ns = nvme_ns(n, nsid);
    if (unlikely(!ns)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (!qemu_uuid_is_null(&ns->params.uuid)) {
        uuid.hdr.nidt = NVME_NIDT_UUID;
        uuid.hdr.nidl = NVME_NIDL_UUID;
        memcpy(uuid.v, ns->params.uuid.data, NVME_NIDL_UUID);
        memcpy(pos, &uuid, sizeof(uuid));
        pos += sizeof(uuid);
    }

    if (!nvme_nguid_is_null(&ns->params.nguid)) {
        nguid.hdr.nidt = NVME_NIDT_NGUID;
        nguid.hdr.nidl = NVME_NIDL_NGUID;
        memcpy(nguid.v, ns->params.nguid.data, NVME_NIDL_NGUID);
        memcpy(pos, &nguid, sizeof(nguid));
        pos += sizeof(nguid);
    }

    if (ns->params.eui64) {
        eui64.hdr.nidt = NVME_NIDT_EUI64;
        eui64.hdr.nidl = NVME_NIDL_EUI64;
        eui64.v = cpu_to_be64(ns->params.eui64);
        memcpy(pos, &eui64, sizeof(eui64));
        pos += sizeof(eui64);
    }

    csi.hdr.nidt = NVME_NIDT_CSI;
    csi.hdr.nidl = NVME_NIDL_CSI;
    csi.v = ns->csi;
    memcpy(pos, &csi, sizeof(csi));
    pos += sizeof(csi);

    return nvme_c2h(n, list, sizeof(list), req);
}

uint16_t nvme_identify_cmd_set(NvmeCtrl *n, NvmeRequest *req, bool _)
{
    uint8_t list[NVME_IDENTIFY_DATA_SIZE] = {};
    static const int data_len = sizeof(list);

    trace_pci_nvme_identify_cmd_set();

    NVME_SET_CSI(*list, NVME_CSI_NVM);
    NVME_SET_CSI(*list, NVME_CSI_ZONED);

    return nvme_c2h(n, list, data_len, req);
}


uint16_t nvme_identify(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeIdentify *c = (NvmeIdentify *)&req->cmd;
    NvmeIdDef *id_op = &n->id_ops.cmds[c->cns];

    trace_pci_nvme_identify(nvme_cid(req), c->cns, le16_to_cpu(c->ctrlid),
                            c->csi);

    if (!id_op->handle) {
        trace_pci_nvme_err_invalid_identify_cns(le32_to_cpu(c->cns));
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    return id_op->handle(n, req, id_op->active);
}

void nvme_identify_23_m(NvmeIdSet *tbl)
{
    tbl->cmds[NVME_ID_CNS_NS] = (NvmeIdDef){
        .handle = nvme_identify_ns,
        .active = true,
    };
    tbl->cmds[NVME_ID_CNS_CTRL] = (NvmeIdDef){
        .handle = nvme_identify_ctrl,
    };
    tbl->cmds[NVME_ID_CNS_NS_ACTIVE_LIST] = (NvmeIdDef){
        .handle = nvme_identify_nslist,
        .active = true,
    };
    tbl->cmds[NVME_ID_CNS_NS_DESCR_LIST] = (NvmeIdDef){
        .handle = nvme_identify_ns_descr_list,
    };
    tbl->cmds[NVME_ID_CNS_CS_NS] = (NvmeIdDef){
        .handle = nvme_identify_ns_csi,
        .active = true,
    };
    tbl->cmds[NVME_ID_CNS_CS_CTRL] = (NvmeIdDef){
        .handle = nvme_identify_ctrl_csi,
    };
    tbl->cmds[NVME_ID_CNS_CS_NS_ACTIVE_LIST] = (NvmeIdDef){
        .handle = nvme_identify_nslist_csi,
        .active = true,
    };
    tbl->cmds[NVME_ID_CNS_CS_IND_NS] = (NvmeIdDef){
        .handle = nvme_identify_ns_ind,
        .active = false,
    };
}

void nvme_identify_defaults(NvmeIdSet *tbl)
{
    nvme_identify_23_m(tbl);

    tbl->cmds[NVME_ID_CNS_NS_PRESENT_LIST] = (NvmeIdDef){
        .handle = nvme_identify_nslist,
        .active = false,
    };
    tbl->cmds[NVME_ID_CNS_NS_PRESENT] = (NvmeIdDef){
        .handle = nvme_identify_ns,
        .active = false,
    };
    tbl->cmds[NVME_ID_CNS_NS_ATTACHED_CTRL_LIST] = (NvmeIdDef){
        .handle = nvme_identify_ctrl_list,
        .active = true,
    };
    tbl->cmds[NVME_ID_CNS_CTRL_LIST] = (NvmeIdDef){
        .handle = nvme_identify_ctrl_list,
        .active = false,
    };
    tbl->cmds[NVME_ID_CNS_PRIMARY_CTRL_CAP] = (NvmeIdDef){
        .handle = nvme_identify_pri_ctrl_cap,
    };
    tbl->cmds[NVME_ID_CNS_SECONDARY_CTRL_LIST] = (NvmeIdDef){
        .handle = nvme_identify_sec_ctrl_list,
    };
    tbl->cmds[NVME_ID_CNS_ENDURANCE_GROUP_LIST] = (NvmeIdDef){
        .handle = nvme_endurance_group_list,
    };
    tbl->cmds[NVME_ID_CNS_CS_NS_PRESENT_LIST] = (NvmeIdDef){
        .handle = nvme_identify_nslist_csi,
        .active = false,
    };
    tbl->cmds[NVME_ID_CNS_CS_NS_PRESENT] = (NvmeIdDef){
        .handle = nvme_identify_ns_csi,
        .active = false,
    };
    tbl->cmds[NVME_ID_CNS_IO_COMMAND_SET] = (NvmeIdDef){
        .handle = nvme_identify_cmd_set,
    };
    tbl->cmds[NVME_ID_CNS_CS_IND_NS_ALLOCATED] = (NvmeIdDef){
        .handle = nvme_identify_ns_ind,
        .active = true,
    };
}
