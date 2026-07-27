#include "qemu/osdep.h"

#include "nvme.h"
#include "log.h"
#include "trace.h"
#include "system/block-backend.h"
#include "qemu/units.h"

typedef struct NvmeCtrl NvmeCtrl;
typedef struct NvmeRequest NvmeRequest;

void nvme_clear_events(NvmeCtrl *n, uint8_t event_type)
{
    NvmeAsyncEvent *event, *next;

    n->aer_mask &= ~(1 << event_type);

    QTAILQ_FOREACH_SAFE(event, &n->aer_queue, entry, next) {
        if (event->result.event_type == event_type) {
            QTAILQ_REMOVE(&n->aer_queue, event, entry);
            n->aer_queued--;
            g_free(event);
        }
    }
}

uint16_t nvme_error_info(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx)
{
    uint32_t trans_len;
    NvmeErrorLog errlog;

    if (ctx->off >= sizeof(errlog)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (!ctx->rae) {
        nvme_clear_events(n, NVME_AER_TYPE_ERROR);
    }

    memset(&errlog, 0x0, sizeof(errlog));
    trans_len = MIN(sizeof(errlog) - ctx->off, ctx->len);

    return nvme_c2h(n, (uint8_t *)&errlog, trans_len, req);
}

struct nvme_stats {
    uint64_t units_read;
    uint64_t units_written;
    uint64_t read_commands;
    uint64_t write_commands;
};

static void nvme_set_blk_stats(NvmeNamespace *ns, struct nvme_stats *stats)
{
    BlockAcctStats *s = blk_get_stats(ns->blkconf.blk);

    stats->units_read += s->nr_bytes[BLOCK_ACCT_READ];
    stats->units_written += s->nr_bytes[BLOCK_ACCT_WRITE];
    stats->read_commands += s->nr_ops[BLOCK_ACCT_READ];
    stats->write_commands += s->nr_ops[BLOCK_ACCT_WRITE];
}

uint16_t nvme_smart_info(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx)
{
    uint32_t nsid = le32_to_cpu(req->cmd.nsid);
    struct nvme_stats stats = { 0 };
    NvmeSmartLog smart = { 0 };
    uint32_t trans_len;
    NvmeNamespace *ns;
    time_t current_ms;
    uint64_t u_read, u_written;

    if (ctx->off >= sizeof(smart)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (nsid != 0xffffffff) {
        ns = nvme_ns(n, nsid);
        if (!ns) {
            return NVME_INVALID_NSID | NVME_DNR;
        }
        nvme_set_blk_stats(ns, &stats);
    } else {
        int i;

        for (i = 1; i <= NVME_MAX_NAMESPACES; i++) {
            ns = nvme_ns(n, i);
            if (!ns) {
                continue;
            }
            nvme_set_blk_stats(ns, &stats);
        }
    }

    trans_len = MIN(sizeof(smart) - ctx->off, ctx->len);
    smart.critical_warning = n->smart_critical_warning;

    u_read = DIV_ROUND_UP(stats.units_read >> BDRV_SECTOR_BITS, 1000);
    u_written = DIV_ROUND_UP(stats.units_written >> BDRV_SECTOR_BITS, 1000);

    smart.data_units_read[0] = cpu_to_le64(u_read);
    smart.data_units_written[0] = cpu_to_le64(u_written);
    smart.host_read_commands[0] = cpu_to_le64(stats.read_commands);
    smart.host_write_commands[0] = cpu_to_le64(stats.write_commands);

    smart.temperature = cpu_to_le16(n->temperature);

    if ((n->temperature >= n->features.temp_thresh_hi) ||
        (n->temperature <= n->features.temp_thresh_low)) {
        smart.critical_warning |= NVME_SMART_TEMPERATURE;
    }

    current_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    smart.power_on_hours[0] =
        cpu_to_le64((((current_ms - n->starttime_ms) / 1000) / 60) / 60);

    if (!ctx->rae) {
        nvme_clear_events(n, NVME_AER_TYPE_SMART);
    }

    return nvme_c2h(n, (uint8_t *) &smart + ctx->off, trans_len, req);
}

uint16_t nvme_fw_log_info(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx)
{
    uint32_t trans_len;
    NvmeFwSlotInfoLog fw_log = {
        .afi = 0x1,
    };

    if (ctx->off >= sizeof(fw_log)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    strpadcpy((char *)&fw_log.frs1, sizeof(fw_log.frs1), "1.0", ' ');
    trans_len = MIN(sizeof(fw_log) - ctx->off, ctx->len);

    return nvme_c2h(n, (uint8_t *) &fw_log + ctx->off, trans_len, req);
}

uint16_t nvme_log_ocp_extended_smart_info(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx)
{
    NvmeNamespace *ns = NULL;
    NvmeSmartLogExtended smart_l = { 0 };
    struct nvme_stats stats = { 0 };
    uint32_t trans_len;

    if (ctx->off >= sizeof(smart_l)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    /* accumulate all stats from all namespaces */
    for (int i = 1; i <= NVME_MAX_NAMESPACES; i++) {
        ns = nvme_ns(n, i);
        if (ns) {
            nvme_set_blk_stats(ns, &stats);
        }
    }

    smart_l.physical_media_units_written[0] = cpu_to_le64(stats.units_written);
    smart_l.physical_media_units_read[0] = cpu_to_le64(stats.units_read);
    smart_l.log_page_version = 0x0005;

    static const uint8_t guid[16] = {
        0xC5, 0xAF, 0x10, 0x28, 0xEA, 0xBF, 0xF2, 0xA4,
        0x9C, 0x4F, 0x6F, 0x7C, 0xC9, 0x14, 0xD5, 0xAF
    };
    memcpy(smart_l.log_page_guid, guid, sizeof(smart_l.log_page_guid));

    if (!ctx->rae) {
        nvme_clear_events(n, NVME_AER_TYPE_SMART);
    }

    trans_len = MIN(sizeof(smart_l) - ctx->off, ctx->len);
    return nvme_c2h(n, (uint8_t *) &smart_l + ctx->off, trans_len, req);
}

uint16_t nvme_changed_nslist(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx)
{
    uint32_t nslist[1024] = {};
    uint32_t trans_len;
    int i = 0;
    uint32_t nsid;

    if (ctx->off >= sizeof(nslist)) {
        trace_pci_nvme_err_invalid_log_page_offset(ctx->off, sizeof(nslist));
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    trans_len = MIN(sizeof(nslist) - ctx->off, ctx->len);

    while ((nsid = find_first_bit(n->changed_nsids, NVME_CHANGED_NSID_SIZE)) !=
            NVME_CHANGED_NSID_SIZE) {
        /*
         * If more than 1024 namespaces, the first entry in the log page should
         * be set to FFFFFFFFh and the others to 0 as spec.
         */
        if (i == ARRAY_SIZE(nslist)) {
            memset(nslist, 0x0, sizeof(nslist));
            nslist[0] = 0xffffffff;
            break;
        }

        nslist[i++] = nsid;
        clear_bit(nsid, n->changed_nsids);
    }

    /*
     * Remove all the remaining list entries in case returns directly due to
     * more than 1024 namespaces.
     */
    if (nslist[0] == 0xffffffff) {
        bitmap_zero(n->changed_nsids, NVME_CHANGED_NSID_SIZE);
    }

    if (!ctx->rae) {
        nvme_clear_events(n, NVME_AER_TYPE_NOTICE);
    }

    return nvme_c2h(n, ((uint8_t *)nslist) + ctx->off, trans_len, req);
}

uint16_t nvme_cmd_effects(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx)
{
    NvmeEffectsLog log = {};
    NvmeCmdSet *iocs = NULL;
    uint32_t trans_len;

    if (ctx->off >= sizeof(log)) {
        trace_pci_nvme_err_invalid_log_page_offset(ctx->off, sizeof(log));
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    switch (NVME_CC_CSS(ldl_le_p(&n->bar.cc))) {
    case NVME_CC_CSS_NVM:
        iocs = &n->cs.iocs.nvm;
        break;

    case NVME_CC_CSS_ALL:
        if (n->cs.iocss[ctx->csi]) {
            iocs = n->cs.iocss[ctx->csi];
        }
        break;
    }

    for (int i = 0; i < 256; i++) {
        log.acs[i] = n->cs.acs.cmds[i].cse;
    }

    if (iocs) {
        for (int i = 0; i < 256; i++) {
            log.iocs[i] = iocs->cmds[i].cse;
        }
    }

    trans_len = MIN(sizeof(log) - ctx->off, ctx->len);

    return nvme_c2h(n, ((uint8_t *)&log) + ctx->off, trans_len, req);
}

uint16_t nvme_endgrp_info(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx)
{
    uint16_t endgrpid = ctx->lspi & 0xffff;
    struct nvme_stats stats = {};
    NvmeEndGrpLog info = {};
    uint64_t trans_len;
    int i;

    if (!n->subsys || endgrpid != 0x1) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (ctx->off >= sizeof(info)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    for (i = 1; i <= NVME_MAX_NAMESPACES; i++) {
        NvmeNamespace *ns = nvme_subsys_ns(n->subsys, i);
        if (!ns) {
            continue;
        }

        nvme_set_blk_stats(ns, &stats);
    }

    info.data_units_read[0] =
        cpu_to_le64(DIV_ROUND_UP(stats.units_read / 1000000000, 1000000000));
    info.data_units_written[0] =
        cpu_to_le64(DIV_ROUND_UP(stats.units_written / 1000000000, 1000000000));
    info.media_units_written[0] =
        cpu_to_le64(DIV_ROUND_UP(stats.units_written / 1000000000, 1000000000));

    info.host_read_commands[0] = cpu_to_le64(stats.read_commands);
    info.host_write_commands[0] = cpu_to_le64(stats.write_commands);

    trans_len = MIN(sizeof(info) - ctx->off, ctx->len);

    return nvme_c2h(n, (uint8_t *)&info + ctx->off, trans_len, req);
}

static size_t sizeof_fdp_conf_descr(size_t nruh, size_t vss)
{
    size_t entry_siz = sizeof(NvmeFdpDescrHdr) + nruh * sizeof(NvmeRuhDescr)
                       + vss;
    return ROUND_UP(entry_siz, 8);
}

uint16_t nvme_fdp_confs(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx)
{
    uint32_t log_size, trans_len;
    g_autofree uint8_t *buf = NULL;
    NvmeFdpDescrHdr *hdr;
    NvmeRuhDescr *ruhd;
    NvmeEnduranceGroup *endgrp;
    NvmeFdpConfsHdr *log;
    size_t nruh, fdp_descr_size;
    uint32_t endgrpid = ctx->lspi;
    int i;

    if (endgrpid != 1 || !n->subsys) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    endgrp = &n->subsys->endgrp;

    if (endgrp->fdp.enabled) {
        nruh = endgrp->fdp.nruh;
    } else {
        nruh = 1;
    }

    fdp_descr_size = sizeof_fdp_conf_descr(nruh, FDPVSS);
    log_size = sizeof(NvmeFdpConfsHdr) + fdp_descr_size;

    if (ctx->off >= log_size) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    trans_len = MIN(log_size - ctx->off, ctx->len);

    buf = g_malloc0(log_size);
    log = (NvmeFdpConfsHdr *)buf;
    hdr = (NvmeFdpDescrHdr *)(log + 1);
    ruhd = (NvmeRuhDescr *)(buf + sizeof(*log) + sizeof(*hdr));

    log->num_confs = cpu_to_le16(0);
    log->size = cpu_to_le32(log_size);

    hdr->descr_size = cpu_to_le16(fdp_descr_size);
    if (endgrp->fdp.enabled) {
        hdr->fdpa = FIELD_DP8(hdr->fdpa, FDPA, VALID, 1);
        hdr->fdpa = FIELD_DP8(hdr->fdpa, FDPA, RGIF, endgrp->fdp.rgif);
        hdr->nrg = cpu_to_le16(endgrp->fdp.nrg);
        hdr->nruh = cpu_to_le16(endgrp->fdp.nruh);
        hdr->maxpids = cpu_to_le16(NVME_FDP_MAXPIDS - 1);
        hdr->nnss = cpu_to_le32(NVME_MAX_NAMESPACES);
        hdr->runs = cpu_to_le64(endgrp->fdp.runs);

        for (i = 0; i < nruh; i++) {
            ruhd->ruht = NVME_RUHT_INITIALLY_ISOLATED;
            ruhd++;
        }
    } else {
        /* 1 bit for RUH in PIF -> 2 RUHs max. */
        hdr->nrg = cpu_to_le16(1);
        hdr->nruh = cpu_to_le16(1);
        hdr->maxpids = cpu_to_le16(NVME_FDP_MAXPIDS - 1);
        hdr->nnss = cpu_to_le32(1);
        hdr->runs = cpu_to_le64(96 * MiB);

        ruhd->ruht = NVME_RUHT_INITIALLY_ISOLATED;
    }

    return nvme_c2h(n, (uint8_t *)buf + ctx->off, trans_len, req);
}

uint16_t nvme_fdp_ruh_usage(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx)
{
    NvmeRuHandle *ruh;
    NvmeRuhuLog *hdr;
    NvmeRuhuDescr *ruhud;
    NvmeEnduranceGroup *endgrp;
    g_autofree uint8_t *buf = NULL;
    uint32_t log_size, trans_len;
    uint16_t i;
    uint32_t endgrpid = ctx->lspi;

    if (endgrpid != 1 || !n->subsys) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    endgrp = &n->subsys->endgrp;

    if (!endgrp->fdp.enabled) {
        return NVME_FDP_DISABLED | NVME_DNR;
    }

    log_size = sizeof(NvmeRuhuLog) + endgrp->fdp.nruh * sizeof(NvmeRuhuDescr);

    if (ctx->off >= log_size) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    trans_len = MIN(log_size - ctx->off, ctx->len);

    buf = g_malloc0(log_size);
    hdr = (NvmeRuhuLog *)buf;
    ruhud = (NvmeRuhuDescr *)(hdr + 1);

    ruh = endgrp->fdp.ruhs;
    hdr->nruh = cpu_to_le16(endgrp->fdp.nruh);

    for (i = 0; i < endgrp->fdp.nruh; i++, ruhud++, ruh++) {
        ruhud->ruha = ruh->ruha;
    }

    return nvme_c2h(n, (uint8_t *)buf + ctx->off, trans_len, req);
}

uint16_t nvme_fdp_stats(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx)
{
    NvmeEnduranceGroup *endgrp;
    NvmeFdpStatsLog log = {};
    uint32_t trans_len;
    uint32_t endgrpid = ctx->lspi;

    if (ctx->off >= sizeof(NvmeFdpStatsLog)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (endgrpid != 1 || !n->subsys) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (!n->subsys->endgrp.fdp.enabled) {
        return NVME_FDP_DISABLED | NVME_DNR;
    }

    endgrp = &n->subsys->endgrp;

    trans_len = MIN(sizeof(log) - ctx->off, ctx->len);

    /* spec value is 128 bit, we only use 64 bit */
    log.hbmw[0] = cpu_to_le64(endgrp->fdp.hbmw);
    log.mbmw[0] = cpu_to_le64(endgrp->fdp.mbmw);
    log.mbe[0] = cpu_to_le64(endgrp->fdp.mbe);

    return nvme_c2h(n, (uint8_t *)&log + ctx->off, trans_len, req);
}

uint16_t nvme_fdp_events(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx)
{
    NvmeEnduranceGroup *endgrp;
    bool host_events = ctx->lsp & 0x1;
    uint32_t log_size, trans_len;
    uint32_t endgrpid = ctx->lspi;
    NvmeFdpEventBuffer *ebuf;
    g_autofree NvmeFdpEventsLog *elog = NULL;
    NvmeFdpEvent *event;

    if (endgrpid != 1 || !n->subsys) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    endgrp = &n->subsys->endgrp;

    if (!endgrp->fdp.enabled) {
        return NVME_FDP_DISABLED | NVME_DNR;
    }

    if (host_events) {
        ebuf = &endgrp->fdp.host_events;
    } else {
        ebuf = &endgrp->fdp.ctrl_events;
    }

    log_size = sizeof(NvmeFdpEventsLog) + ebuf->nelems * sizeof(NvmeFdpEvent);

    if (ctx->off >= log_size) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    trans_len = MIN(log_size - ctx->off, ctx->len);
    elog = g_malloc0(log_size);
    elog->num_events = cpu_to_le32(ebuf->nelems);
    event = (NvmeFdpEvent *)(elog + 1);

    if (ebuf->nelems && ebuf->start == ebuf->next) {
        unsigned int nelems = (NVME_FDP_MAX_EVENTS - ebuf->start);
        /* wrap over, copy [start;NVME_FDP_MAX_EVENTS[ and [0; next[ */
        memcpy(event, &ebuf->events[ebuf->start],
               sizeof(NvmeFdpEvent) * nelems);
        memcpy(event + nelems, ebuf->events,
               sizeof(NvmeFdpEvent) * ebuf->next);
    } else if (ebuf->start < ebuf->next) {
        memcpy(event, &ebuf->events[ebuf->start],
               sizeof(NvmeFdpEvent) * (ebuf->next - ebuf->start));
    }

    return nvme_c2h(n, (uint8_t *)elog + ctx->off, trans_len, req);
}

void nvme_init_ctrl_log_io_adm_14m(NvmeLogSet *ls)
{
    ls->cmds[NVME_LOG_ERROR_INFO] = &nvme_error_info;
    ls->cmds[NVME_LOG_SMART_INFO] = &nvme_smart_info;
    ls->cmds[NVME_LOG_FW_SLOT_INFO] = &nvme_fw_log_info;
    ls->cmds[NVME_LOG_CMD_EFFECTS] = &nvme_cmd_effects;
}

void nvme_init_ctrl_log_default(NvmeLogSet *ls)
{
    nvme_init_ctrl_log_io_adm_14m(ls);

    ls->cmds[NVME_LOG_CHANGED_NSLIST] = &nvme_changed_nslist;
    ls->cmds[NVME_LOG_ENDGRP] = &nvme_endgrp_info;
    ls->cmds[NVME_LOG_FDP_CONFS] = &nvme_fdp_confs;
    ls->cmds[NVME_LOG_FDP_RUH_USAGE] = &nvme_fdp_ruh_usage;
    ls->cmds[NVME_LOG_FDP_STATS] = &nvme_fdp_stats;
    ls->cmds[NVME_LOG_FDP_EVENTS] = &nvme_fdp_events;
    /* vendor-specific start */
    ls->cmds[NVME_OCP_EXTENDED_SMART_INFO] = &nvme_log_ocp_extended_smart_info;
    /* vendor-specific end */
}

uint16_t nvme_get_log(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeCmd *cmd = &req->cmd;

    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t dw11 = le32_to_cpu(cmd->cdw11);
    uint32_t dw12 = le32_to_cpu(cmd->cdw12);
    uint32_t dw13 = le32_to_cpu(cmd->cdw13);
    NvmeLogRqCtx ctx = {
        .lspi = (dw11 >> 16),
        .lid = dw10 & 0xff,
        .lsp = (dw10 >> 8) & 0xf,
        .rae = (dw10 >> 15) & 0x1,
        .csi = le32_to_cpu(cmd->cdw14) >> 24,
    };
    uint32_t numdl, numdu;
    uint64_t lpol, lpou;
    uint16_t status;
    NvmeLogFn op = n->log_ops.cmds[ctx.lid];

    if (unlikely(!op)) {
        trace_pci_nvme_err_invalid_log_page(nvme_cid(req), ctx.lid);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    numdl = (dw10 >> 16);
    numdu = (dw11 & 0xffff);
    lpol = dw12;
    lpou = dw13;

    ctx.len = (((numdu << 16) | numdl) + 1) << 2;
    ctx.off = (lpou << 32ULL) | lpol;

    if (ctx.off & 0x3) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    trace_pci_nvme_get_log(nvme_cid(req), ctx.lid, ctx.lsp, ctx.rae, ctx.len, ctx.off);

    status = nvme_check_mdts(n, ctx.len);
    if (status) {
        return status;
    }

    return op(n, req, &ctx);
}

