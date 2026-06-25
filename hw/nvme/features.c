#include "qemu/osdep.h"
#include "system/block-backend.h"

#include "nvme.h"
#include "features.h"
#include "trace.h"

bool nvme_feature_supported(NvmeCtrl *n, uint8_t fid)
{
    return n->features.defs[fid].get != NULL;
}

uint16_t nvme_get_feature_timestamp(NvmeCtrl *n, NvmeRequest *req, bool defval)
{
    uint64_t timestamp = nvme_get_timestamp(n);

    return nvme_c2h(n, (uint8_t *)&timestamp, sizeof(timestamp), req);
}

uint16_t nvme_get_feature_arbitration(NvmeCtrl *n, NvmeRequest *req, bool defval)
{
    req->cqe.result = cpu_to_le32(NVME_ARB_AB_NOLIMIT);
    return NVME_SUCCESS;
}

uint16_t nvme_get_feature_temp_threshold(NvmeCtrl *n, NvmeRequest *req, bool defval)
{
    uint32_t dw11 = le32_to_cpu(req->cmd.cdw11);
    uint32_t result = 0;

    /*
     * The controller only implements the Composite Temperature sensor, so
     * return 0 for all other sensors.
     */
    if (NVME_TEMP_TMPSEL(dw11) != NVME_TEMP_TMPSEL_COMPOSITE) {
        goto out;
    }

    if (defval) {
        if (NVME_TEMP_THSEL(dw11) == NVME_TEMP_THSEL_OVER) {
            result = NVME_TEMPERATURE_WARNING;
        }
        goto out;
    }

    switch (NVME_TEMP_THSEL(dw11)) {
    case NVME_TEMP_THSEL_OVER:
        result = n->features.temp_thresh_hi;
        break;
    case NVME_TEMP_THSEL_UNDER:
        result = n->features.temp_thresh_low;
        break;
    default:
        return NVME_INVALID_FIELD | NVME_DNR;
    }

out:
    req->cqe.result = cpu_to_le32(result);
    return NVME_SUCCESS;
}

uint16_t nvme_get_feature_errrec(NvmeCtrl *n, NvmeRequest *req, bool defval)
{
    NvmeNamespace *ns = NULL;
    uint32_t nsid = le32_to_cpu(req->cmd.nsid);

    if (unlikely(defval)) {
        return NVME_SUCCESS;
    }

    ns = nvme_ns(n, nsid);
    assert(ns); /* caller already checked */

    req->cqe.result = cpu_to_le32(ns->features.err_rec);
    return NVME_SUCCESS;
}

uint16_t nvme_get_feature_vwc(NvmeCtrl *n, NvmeRequest *req, bool defval)
{
    NvmeNamespace *ns = NULL;
    uint32_t result = 0;

    if (unlikely(defval)) {
        return NVME_SUCCESS;
    }

    for (int i = 1; i <= NVME_MAX_NAMESPACES; i++) {
        ns = nvme_ns(n, i);
        if (!ns) {
            continue;
        }

        result = blk_enable_write_cache(ns->blkconf.blk);
        if (result) {
            req->cqe.result = cpu_to_le32(result);
            break;
        }
    }

    trace_pci_nvme_getfeat_vwcache(result ? "enabled" : "disabled");
    return NVME_SUCCESS;
}

uint16_t nvme_get_feature_numqs(NvmeCtrl *n, NvmeRequest *req, bool defval)
{
    uint32_t result = (n->conf_ioqpairs - 1) | ((n->conf_ioqpairs - 1) << 16);
    req->cqe.result = cpu_to_le32(result);
    trace_pci_nvme_getfeat_numq(result);
    return NVME_SUCCESS;
}

uint16_t nvme_get_feature_int_vec_conf(NvmeCtrl *n, NvmeRequest *req, bool defval)
{
    uint32_t dw11 = le32_to_cpu(req->cmd.cdw11);
    uint16_t iv = dw11 & 0xffff;
    uint32_t result = 0;

    if (iv >= n->conf_ioqpairs + 1) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    result = iv;
    if (iv == n->admin_cq.vector) {
        result |= NVME_INTVC_NOCOALESCING;
    }

    req->cqe.result = cpu_to_le32(result);
    return NVME_SUCCESS;
}

uint16_t nvme_get_feature_write_atomicity(NvmeCtrl *n, NvmeRequest *req,
                                                 bool defval)
{
    req->cqe.result = cpu_to_le32(n->dn);
    return NVME_SUCCESS;
}

uint16_t nvme_get_feature_aec(NvmeCtrl *n, NvmeRequest *req, bool defval)
{
    if (likely(!defval)) {
        req->cqe.result = cpu_to_le32(n->features.async_config);
    }
    return NVME_SUCCESS;
}

uint16_t nvme_get_feature_hbs(NvmeCtrl *n, NvmeRequest *req, bool defval)
{
        return nvme_c2h(n, (uint8_t *)&n->features.hbs,
                        sizeof(n->features.hbs), req);
}

uint16_t nvme_get_feature_fdp(NvmeCtrl *n, NvmeRequest *req, bool defval)
{
    uint32_t dw11 = le32_to_cpu(req->cmd.cdw11);
    uint16_t endgrpid = dw11 & 0xff;
    uint32_t result = 0;

    if (endgrpid != 0x1) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (!n->subsys || !n->subsys->endgrp.fdp.enabled) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    result = FIELD_DP16(0, FEAT_FDP, FDPE, 1);
    result = FIELD_DP16(result, FEAT_FDP, CONF_NDX, 0);
    req->cqe.result = cpu_to_le32(result);

    return NVME_SUCCESS;
}

uint16_t nvme_get_feature_fdp_events(NvmeCtrl *n, NvmeRequest *req, bool defval)
{
    NvmeCmd *cmd = &req->cmd;
    uint32_t cdw11 = le32_to_cpu(cmd->cdw11);
    uint16_t ph = cdw11 & 0xffff;
    uint8_t noet = (cdw11 >> 16) & 0xff;
    uint16_t ruhid, ret;
    uint32_t nentries = 0;
    uint8_t s_events_ndx = 0;
    size_t s_events_siz = sizeof(NvmeFdpEventDescr) * noet;
    g_autofree NvmeFdpEventDescr *s_events = g_malloc0(s_events_siz);
    NvmeRuHandle *ruh;
    NvmeFdpEventDescr *s_event;
    NvmeNamespace *ns = NULL;

    if (unlikely(defval)) {
        return NVME_SUCCESS;
    }

    ns = nvme_ns(n, cmd->nsid);
    assert(ns); /* caller checked */

    if (!n->subsys || !n->subsys->endgrp.fdp.enabled) {
        return NVME_FDP_DISABLED | NVME_DNR;
    }

    if (!nvme_ph_valid(ns, ph)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    ruhid = ns->fdp.phs[ph];
    ruh = &n->subsys->endgrp.fdp.ruhs[ruhid];

    assert(ruh);

    if (unlikely(noet == 0)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    for (uint8_t event_type = 0; event_type < FDP_EVT_MAX; event_type++) {
        uint8_t shift = nvme_fdp_evf_shifts[event_type];
        if (!shift && event_type) {
            /*
             * only first entry (event_type == 0) has a shift value of 0
             * other entries are simply unpopulated.
             */
            continue;
        }

        nentries++;

        s_event = &s_events[s_events_ndx];
        s_event->evt = event_type;
        s_event->evta = (ruh->event_filter >> shift) & 0x1;

        /* break if all `noet` entries are filled */
        if ((++s_events_ndx) == noet) {
            break;
        }
    }

    ret = nvme_c2h(n, s_events, s_events_siz, req);
    if (ret) {
        return ret;
    }

    req->cqe.result = cpu_to_le32(nentries);
    return NVME_SUCCESS;
}

uint16_t nvme_get_feature_noop(NvmeCtrl *n, NvmeRequest *req, bool defval)
{
    return NVME_SUCCESS;
}

uint16_t nvme_get_feature(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeCmd *cmd = &req->cmd;
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t dw11 = le32_to_cpu(cmd->cdw11);
    uint32_t nsid = le32_to_cpu(cmd->nsid);
    uint8_t fid = NVME_GETSETFEAT_FID(dw10);
    NvmeGetFeatureSelect sel = NVME_GETFEAT_SELECT(dw10);
    NvmeFeatureDef *feat = NULL;
    bool defval = false;

    trace_pci_nvme_getfeat(nvme_cid(req), nsid, fid, sel, dw11);

    if (!nvme_feature_supported(n, fid)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    feat = &n->features.defs[fid];

    if (feat->cap & NVME_FEAT_CAP_NS) {
        if (!nvme_nsid_valid(n, nsid) || nsid == NVME_NSID_BROADCAST) {
            /*
             * The Reservation Notification Mask and Reservation Persistence
             * features require a status code of Invalid Field in Command when
             * NSID is FFFFFFFFh. Since the device does not support those
             * features we can always return Invalid Namespace or Format as we
             * should do for all other features.
             */
            return NVME_INVALID_NSID | NVME_DNR;
        }

        if (!nvme_ns(n, nsid)) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }
    }

    switch (sel) {
        case NVME_GETFEAT_SELECT_CURRENT:
            break;
        case NVME_GETFEAT_SELECT_SAVED:
        /* no features are saveable by the controller; fallthrough */
        case NVME_GETFEAT_SELECT_DEFAULT:
            defval = true;
            break;
        case NVME_GETFEAT_SELECT_CAP:
            req->cqe.result = cpu_to_le32(feat->cap);
            return NVME_SUCCESS;
        default:
            return NVME_INVALID_FIELD | NVME_DNR;
    }

    return feat->get(n, req, defval);
}

uint16_t nvme_set_feature_timestamp(NvmeCtrl *n, NvmeRequest *req)
{
    uint16_t ret;
    uint64_t timestamp;

    ret = nvme_h2c(n, (uint8_t *)&timestamp, sizeof(timestamp), req);
    if (ret) {
        return ret;
    }

    nvme_set_timestamp(n, timestamp);

    return NVME_SUCCESS;
}

uint16_t nvme_set_feature_fdp_events(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeCmd *cmd = &req->cmd;
    uint32_t cdw11 = le32_to_cpu(cmd->cdw11);
    uint16_t ph = cdw11 & 0xffff;
    uint8_t noet = (cdw11 >> 16) & 0xff;
    uint16_t ret, ruhid;
    uint8_t enable = le32_to_cpu(cmd->cdw12) & 0x1;
    uint8_t event_mask = 0;
    unsigned int i;
    g_autofree uint8_t *events = g_malloc0(noet);
    NvmeRuHandle *ruh = NULL;
    uint32_t nsid = le32_to_cpu(cmd->nsid);
    NvmeNamespace *ns = nvme_ns(n, nsid);

    assert(ns); /* caller checked */

    if (!n->subsys || !n->subsys->endgrp.fdp.enabled) {
        return NVME_FDP_DISABLED | NVME_DNR;
    }

    if (!nvme_ph_valid(ns, ph)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    ruhid = ns->fdp.phs[ph];
    ruh = &n->subsys->endgrp.fdp.ruhs[ruhid];

    ret = nvme_h2c(n, events, noet, req);
    if (ret) {
        return ret;
    }

    for (i = 0; i < noet; i++) {
        event_mask |= (1 << nvme_fdp_evf_shifts[events[i]]);
    }

    if (enable) {
        ruh->event_filter |= event_mask;
    } else {
        ruh->event_filter = ruh->event_filter & ~event_mask;
    }

    return NVME_SUCCESS;
}

uint16_t nvme_set_feature_write_atomicity(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeCmd *cmd = &req->cmd;

    uint32_t dw11 = le32_to_cpu(cmd->cdw11);

    uint16_t awun = le16_to_cpu(n->id_ctrl.awun);
    uint16_t awupf = le16_to_cpu(n->id_ctrl.awupf);

    n->dn = dw11 & 0x1;

    nvme_atomic_configure_max_write_size(n->dn, awun, awupf, &n->atomic);

    for (int i = 1; i <= NVME_MAX_NAMESPACES; i++) {
        uint16_t nawun, nawupf, nabsn, nabspf;

        NvmeNamespace *ns = nvme_ns(n, i);
        if (!ns) {
            continue;
        }

        nawun = le16_to_cpu(ns->id_ns.nawun);
        nawupf = le16_to_cpu(ns->id_ns.nawupf);

        nvme_atomic_configure_max_write_size(n->dn, nawun, nawupf,
                                             &ns->atomic);

        nabsn = le16_to_cpu(ns->id_ns.nabsn);
        nabspf = le16_to_cpu(ns->id_ns.nabspf);

        nvme_ns_atomic_configure_boundary(n->dn, nabsn, nabspf,
                                          &ns->atomic);
    }

    return NVME_SUCCESS;
}

uint16_t nvme_set_feature_temp_threshold(NvmeCtrl *n, NvmeRequest *req)
{
    uint32_t dw11 = le32_to_cpu(req->cmd.cdw11);
    uint32_t result = 0;

    /* the controller only implements the composite temperature sensor */
    if (NVME_TEMP_TMPSEL(dw11) != NVME_TEMP_TMPSEL_COMPOSITE) {
        goto out;
    }

    switch (NVME_TEMP_THSEL(dw11)) {
    case NVME_TEMP_THSEL_OVER:
        n->features.temp_thresh_hi = NVME_TEMP_TMPTH(dw11);
        break;
    case NVME_TEMP_THSEL_UNDER:
        n->features.temp_thresh_low = NVME_TEMP_TMPTH(dw11);
        break;
    default:
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if ((n->temperature >= n->features.temp_thresh_hi) ||
        (n->temperature <= n->features.temp_thresh_low)) {
        nvme_smart_event(n, NVME_SMART_TEMPERATURE);
    }

out:
    req->cqe.result = cpu_to_le32(result);
    return NVME_SUCCESS;
}

uint16_t nvme_set_feature_errrec(NvmeCtrl *n, NvmeRequest *req)
{
    uint32_t nsid = le32_to_cpu(req->cmd.nsid);
    uint32_t dw11 = le32_to_cpu(req->cmd.cdw11);

    NvmeNamespace *ns = NULL;

    if (nsid == NVME_NSID_BROADCAST) {
        for (int i = 1; i <= NVME_MAX_NAMESPACES; i++) {
            ns = nvme_ns(n, i);

            if (!ns) {
                continue;
            }

            if (NVME_ID_NS_NSFEAT_DULBE(ns->id_ns.nsfeat)) {
                ns->features.err_rec = dw11;
            }
        }
    } else {
        ns = nvme_ns(n, nsid);

        assert(ns); /* caller checked */
        if (NVME_ID_NS_NSFEAT_DULBE(ns->id_ns.nsfeat))  {
            ns->features.err_rec = dw11;
        }
    }

    return NVME_SUCCESS;
}

uint16_t nvme_set_feature_vwc(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeNamespace *ns = NULL;

    uint32_t dw11 = le32_to_cpu(req->cmd.cdw11);

    for (int i = 1; i <= NVME_MAX_NAMESPACES; i++) {
        ns = nvme_ns(n, i);
        if (!ns) {
            continue;
        }

        if (!(dw11 & 0x1) && blk_enable_write_cache(ns->blkconf.blk)) {
            blk_flush(ns->blkconf.blk);
        }

        blk_set_enable_write_cache(ns->blkconf.blk, dw11 & 1);
    }

    return NVME_SUCCESS;
}

uint16_t nvme_set_feature_numqs(NvmeCtrl *n, NvmeRequest *req)
{
    uint32_t dw11 = le32_to_cpu(req->cmd.cdw11);

    if (n->qs_created) {
        return NVME_CMD_SEQ_ERROR | NVME_DNR;
    }

    /*
     * NVMe v1.3, Section 5.21.1.7: FFFFh is not an allowed value for NCQR
     * and NSQR.
     */
    if ((dw11 & 0xffff) == 0xffff || ((dw11 >> 16) & 0xffff) == 0xffff) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    trace_pci_nvme_setfeat_numq((dw11 & 0xffff) + 1,
                                ((dw11 >> 16) & 0xffff) + 1,
                                n->conf_ioqpairs,
                                n->conf_ioqpairs);
    req->cqe.result = cpu_to_le32((n->conf_ioqpairs - 1) |
                                  ((n->conf_ioqpairs - 1) << 16));

    return NVME_SUCCESS;
}

uint16_t nvme_set_feature_aec(NvmeCtrl *n, NvmeRequest *req)
{
    n->features.async_config = le32_to_cpu(req->cmd.cdw11);
    return NVME_SUCCESS;
}

uint16_t nvme_set_feature_hbs(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeNamespace *ns = NULL;

    uint16_t status = nvme_h2c(n, (uint8_t *)&n->features.hbs,
                      sizeof(n->features.hbs), req);
    if (status) {
        return status;
    }

    for (int i = 1; i <= NVME_MAX_NAMESPACES; i++) {
        ns = nvme_ns(n, i);

        if (!ns) {
            continue;
        }

        ns->id_ns.nlbaf = ns->nlbaf - 1;
        if (!n->features.hbs.lbafee) {
            ns->id_ns.nlbaf = MIN(ns->id_ns.nlbaf, 15);
        }
    }

    return status;
}

uint16_t nvme_set_feature_cmd_set_profile(NvmeCtrl *n, NvmeRequest *req)
{
    uint32_t dw11 = le32_to_cpu(req->cmd.cdw11);

    if (dw11 & 0x1ff) {
        trace_pci_nvme_err_invalid_iocsci(dw11 & 0x1ff);
        return NVME_IOCS_COMBINATION_REJECTED | NVME_DNR;
    }

    return NVME_SUCCESS;
}

uint16_t nvme_set_feature_fdp(NvmeCtrl *n, NvmeRequest *req)
{
    /* spec: abort with cmd seq err if there's one or more NS' in endgrp */
    return NVME_CMD_SEQ_ERROR | NVME_DNR;
}

uint16_t nvme_set_feature(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeNamespace *ns = NULL;

    NvmeCmd *cmd = &req->cmd;
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t dw11 = le32_to_cpu(cmd->cdw11);
    uint32_t nsid = le32_to_cpu(cmd->nsid);
    uint8_t fid = NVME_GETSETFEAT_FID(dw10);
    uint8_t save = NVME_SETFEAT_SAVE(dw10);
    NvmeFeatureDef *feat = NULL;

    trace_pci_nvme_setfeat(nvme_cid(req), nsid, fid, save, dw11);

    feat = &n->features.defs[fid];

    if (save && !(feat->cap & NVME_FEAT_CAP_SAVE)) {
        return NVME_FID_NOT_SAVEABLE | NVME_DNR;
    }

    if (!nvme_feature_supported(n, fid)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (feat->cap & NVME_FEAT_CAP_NS) {
        if (nsid != NVME_NSID_BROADCAST) {
            if (!nvme_nsid_valid(n, nsid)) {
                return NVME_INVALID_NSID | NVME_DNR;
            }

            ns = nvme_ns(n, nsid);
            if (unlikely(!ns)) {
                return NVME_INVALID_FIELD | NVME_DNR;
            }
        }
    } else if (nsid && nsid != NVME_NSID_BROADCAST) {
        if (!nvme_nsid_valid(n, nsid)) {
            return NVME_INVALID_NSID | NVME_DNR;
        }

        return NVME_FEAT_NOT_NS_SPEC | NVME_DNR;
    }

    if (!(feat->cap & NVME_FEAT_CAP_CHANGE)) {
        return NVME_FEAT_NOT_CHANGEABLE | NVME_DNR;
    }

    assert(feat->set);
    return feat->set(n, req);
}
