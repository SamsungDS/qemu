#include "qemu/osdep.h"
#include "system/block-backend.h"
#include "hw/pci/msix.h"

#include "nvme.h"
#include "cs_adm.h"
#include "features.h"
#include "identify.h"
#include "log.h"
#include "trace.h"

uint16_t nvme_del_sq(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeDeleteQ *c = (NvmeDeleteQ *)&req->cmd;
    NvmeRequest *r, *next;
    NvmeSQueue *sq;
    NvmeCQueue *cq;
    uint16_t qid = le16_to_cpu(c->qid);

    if (unlikely(!qid || nvme_check_sqid(n, qid))) {
        trace_pci_nvme_err_invalid_del_sq(qid);
        return NVME_INVALID_QID | NVME_DNR;
    }

    trace_pci_nvme_del_sq(qid);

    sq = n->sq[qid];
    while (!QTAILQ_EMPTY(&sq->out_req_list)) {
        r = QTAILQ_FIRST(&sq->out_req_list);
        assert(r->aiocb);
        r->status = NVME_CMD_ABORT_SQ_DEL;
        blk_aio_cancel(r->aiocb);
    }

    assert(QTAILQ_EMPTY(&sq->out_req_list));

    if (!nvme_check_cqid(n, sq->cqid)) {
        cq = n->cq[sq->cqid];
        QTAILQ_REMOVE(&cq->sq_list, sq, entry);

        nvme_post_cqes(cq);
        QTAILQ_FOREACH_SAFE(r, &cq->req_list, entry, next) {
            if (r->sq == sq) {
                QTAILQ_REMOVE(&cq->req_list, r, entry);
                QTAILQ_INSERT_TAIL(&sq->req_list, r, entry);
            }
        }
    }

    nvme_free_sq(sq, n);
    return NVME_SUCCESS;
}

uint16_t nvme_create_sq(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeSQueue *sq;
    NvmeCreateSq *c = (NvmeCreateSq *)&req->cmd;

    uint16_t cqid = le16_to_cpu(c->cqid);
    uint16_t sqid = le16_to_cpu(c->sqid);
    uint16_t qsize = le16_to_cpu(c->qsize);
    uint16_t qflags = le16_to_cpu(c->sq_flags);
    uint64_t prp1 = le64_to_cpu(c->prp1);

    trace_pci_nvme_create_sq(prp1, sqid, cqid, qsize, qflags);

    if (unlikely(!cqid || nvme_check_cqid(n, cqid))) {
        trace_pci_nvme_err_invalid_create_sq_cqid(cqid);
        return NVME_INVALID_CQID | NVME_DNR;
    }
    if (unlikely(!sqid || sqid > n->conf_ioqpairs || n->sq[sqid] != NULL)) {
        trace_pci_nvme_err_invalid_create_sq_sqid(sqid);
        return NVME_INVALID_QID | NVME_DNR;
    }
    if (unlikely(!qsize || qsize > NVME_CAP_MQES(ldq_le_p(&n->bar.cap)))) {
        trace_pci_nvme_err_invalid_create_sq_size(qsize);
        return NVME_MAX_QSIZE_EXCEEDED | NVME_DNR;
    }
    if (unlikely(prp1 & (n->page_size - 1))) {
        trace_pci_nvme_err_invalid_create_sq_addr(prp1);
        return NVME_INVALID_PRP_OFFSET | NVME_DNR;
    }
    if (unlikely(!(NVME_SQ_FLAGS_PC(qflags)))) {
        trace_pci_nvme_err_invalid_create_sq_qflags(NVME_SQ_FLAGS_PC(qflags));
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    sq = g_malloc0(sizeof(*sq));
    nvme_init_sq(sq, n, prp1, sqid, cqid, qsize + 1);
    return NVME_SUCCESS;
}

uint16_t nvme_del_cq(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeDeleteQ *c = (NvmeDeleteQ *)&req->cmd;
    NvmeCQueue *cq;
    uint16_t qid = le16_to_cpu(c->qid);

    if (unlikely(!qid || nvme_check_cqid(n, qid))) {
        trace_pci_nvme_err_invalid_del_cq_cqid(qid);
        return NVME_INVALID_CQID | NVME_DNR;
    }

    cq = n->cq[qid];
    if (unlikely(!QTAILQ_EMPTY(&cq->sq_list))) {
        trace_pci_nvme_err_invalid_del_cq_notempty(qid);
        return NVME_INVALID_QUEUE_DEL;
    }

    if (cq->irq_enabled && cq->tail != cq->head) {
        n->cq_pending--;
    }

    nvme_irq_deassert(n, cq);
    trace_pci_nvme_del_cq(qid);
    nvme_free_cq(cq, n);
    return NVME_SUCCESS;
}

uint16_t nvme_create_cq(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeCQueue *cq;
    NvmeCreateCq *c = (NvmeCreateCq *)&req->cmd;
    uint16_t cqid = le16_to_cpu(c->cqid);
    uint16_t vector = le16_to_cpu(c->irq_vector);
    uint16_t qsize = le16_to_cpu(c->qsize);
    uint16_t qflags = le16_to_cpu(c->cq_flags);
    uint64_t prp1 = le64_to_cpu(c->prp1);
    uint32_t cc = ldq_le_p(&n->bar.cc);
    uint8_t iocqes = NVME_CC_IOCQES(cc);
    uint8_t iosqes = NVME_CC_IOSQES(cc);

    trace_pci_nvme_create_cq(prp1, cqid, vector, qsize, qflags,
                             NVME_CQ_FLAGS_IEN(qflags) != 0);

    if (iosqes != NVME_SQES || iocqes != NVME_CQES) {
        trace_pci_nvme_err_invalid_create_cq_entry_size(iosqes, iocqes);
        return NVME_MAX_QSIZE_EXCEEDED | NVME_DNR;
    }

    if (unlikely(!cqid || cqid > n->conf_ioqpairs || n->cq[cqid] != NULL)) {
        trace_pci_nvme_err_invalid_create_cq_cqid(cqid);
        return NVME_INVALID_QID | NVME_DNR;
    }
    if (unlikely(!qsize || qsize > NVME_CAP_MQES(ldq_le_p(&n->bar.cap)))) {
        trace_pci_nvme_err_invalid_create_cq_size(qsize);
        return NVME_MAX_QSIZE_EXCEEDED | NVME_DNR;
    }
    if (unlikely(prp1 & (n->page_size - 1))) {
        trace_pci_nvme_err_invalid_create_cq_addr(prp1);
        return NVME_INVALID_PRP_OFFSET | NVME_DNR;
    }
    if (unlikely(!msix_enabled(PCI_DEVICE(n)) && vector)) {
        trace_pci_nvme_err_invalid_create_cq_vector(vector);
        return NVME_INVALID_IRQ_VECTOR | NVME_DNR;
    }
    if (unlikely(vector >= n->conf_msix_qsize)) {
        trace_pci_nvme_err_invalid_create_cq_vector(vector);
        return NVME_INVALID_IRQ_VECTOR | NVME_DNR;
    }
    if (unlikely(!(NVME_CQ_FLAGS_PC(qflags)))) {
        trace_pci_nvme_err_invalid_create_cq_qflags(NVME_CQ_FLAGS_PC(qflags));
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    cq = g_malloc0(sizeof(*cq));
    nvme_init_cq(cq, n, prp1, cqid, vector, qsize + 1,
                 NVME_CQ_FLAGS_IEN(qflags));

    /*
     * It is only required to set qs_created when creating a completion queue;
     * creating a submission queue without a matching completion queue will
     * fail.
     */
    n->qs_created = true;
    return NVME_SUCCESS;
}

uint16_t nvme_abort(NvmeCtrl *n, NvmeRequest *req)
{
    uint16_t sqid = le32_to_cpu(req->cmd.cdw10) & 0xffff;
    uint16_t cid  = (le32_to_cpu(req->cmd.cdw10) >> 16) & 0xffff;
    NvmeSQueue *sq;
    NvmeRequest *r, *next;
    int i;

    req->cqe.result = 1;
    if (nvme_check_sqid(n, sqid)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    sq = n->sq[sqid];

    if (sqid == 0) {
        for (i = 0; i < n->outstanding_aers; i++) {
            NvmeRequest *re = n->aer_reqs[i];
            if (re->cqe.cid == cid) {
                memmove(n->aer_reqs + i, n->aer_reqs + i + 1,
                         (n->outstanding_aers - i - 1) * sizeof(NvmeRequest *));
                n->outstanding_aers--;
                re->status = NVME_CMD_ABORT_REQ;
                req->cqe.result = 0;
                nvme_enqueue_req_completion(&n->admin_cq, re);
                return NVME_SUCCESS;
            }
        }
    }

    QTAILQ_FOREACH_SAFE(r, &sq->out_req_list, entry, next) {
        if (r->cqe.cid == cid) {
            if (r->aiocb) {
                r->status = NVME_CMD_ABORT_REQ;
                blk_aio_cancel_async(r->aiocb);
            }
            break;
        }
    }

    return NVME_SUCCESS;
}

uint16_t nvme_aer(NvmeCtrl *n, NvmeRequest *req)
{
    trace_pci_nvme_aer(nvme_cid(req));

    if (n->outstanding_aers > n->params.aerl) {
        trace_pci_nvme_aer_aerl_exceeded();
        return NVME_AER_LIMIT_EXCEEDED;
    }

    n->aer_reqs[n->outstanding_aers] = req;
    n->outstanding_aers++;

    if (!QTAILQ_EMPTY(&n->aer_queue)) {
        nvme_process_aers(n);
    }

    return NVME_NO_COMPLETE;
}

uint16_t nvme_ns_attachment(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeNamespace *ns;
    NvmeCtrl *ctrl;
    uint16_t list[NVME_CONTROLLER_LIST_SIZE] = {};
    uint32_t nsid = le32_to_cpu(req->cmd.nsid);
    uint32_t dw10 = le32_to_cpu(req->cmd.cdw10);
    uint8_t sel = dw10 & 0xf;
    uint16_t *nr_ids = &list[0];
    uint16_t *ids = &list[1];
    uint16_t ret;
    int i;

    trace_pci_nvme_ns_attachment(nvme_cid(req), dw10 & 0xf);

    if (!nvme_nsid_valid(n, nsid)) {
        return NVME_INVALID_NSID | NVME_DNR;
    }

    ns = nvme_subsys_ns(n->subsys, nsid);
    if (!ns) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    ret = nvme_h2c(n, (uint8_t *)list, 4096, req);
    if (ret) {
        return ret;
    }

    if (!*nr_ids) {
        return NVME_NS_CTRL_LIST_INVALID | NVME_DNR;
    }

    *nr_ids = MIN(*nr_ids, NVME_CONTROLLER_LIST_SIZE - 1);
    for (i = 0; i < *nr_ids; i++) {
        ctrl = nvme_subsys_ctrl(n->subsys, ids[i]);
        if (!ctrl) {
            return NVME_NS_CTRL_LIST_INVALID | NVME_DNR;
        }

        switch (sel) {
        case NVME_NS_ATTACHMENT_ATTACH:
            if (nvme_ns(ctrl, nsid)) {
                return NVME_NS_ALREADY_ATTACHED | NVME_DNR;
            }

            if (ns->attached && !ns->params.shared) {
                return NVME_NS_PRIVATE | NVME_DNR;
            }

            if (!nvme_csi_supported(ctrl, ns->csi)) {
                return NVME_IOCS_NOT_SUPPORTED | NVME_DNR;
            }

            nvme_attach_ns(ctrl, ns);
            nvme_update_dsm_limits(ctrl, ns);

            break;

        case NVME_NS_ATTACHMENT_DETACH:
            if (!nvme_ns(ctrl, nsid)) {
                return NVME_NS_NOT_ATTACHED | NVME_DNR;
            }

            nvme_detach_ns(ctrl, ns);
            nvme_update_dsm_limits(ctrl, NULL);

            break;

        default:
            return NVME_INVALID_FIELD | NVME_DNR;
        }

        /*
         * Add namespace id to the changed namespace id list for event clearing
         * via Get Log Page command.
         */
        if (!test_and_set_bit(nsid, ctrl->changed_nsids)) {
            nvme_enqueue_event(ctrl, NVME_AER_TYPE_NOTICE,
                               NVME_AER_INFO_NOTICE_NS_ATTR_CHANGED,
                               NVME_LOG_CHANGED_NSLIST);
        }
    }

    return NVME_SUCCESS;
}

uint16_t nvme_directive_send(NvmeCtrl *n, NvmeRequest *req)
{
    return NVME_INVALID_FIELD | NVME_DNR;
}

uint16_t nvme_virt_mngmt(NvmeCtrl *n, NvmeRequest *req)
{
    uint32_t dw10 = le32_to_cpu(req->cmd.cdw10);
    uint32_t dw11 = le32_to_cpu(req->cmd.cdw11);
    uint8_t act = dw10 & 0xf;
    uint8_t rt = (dw10 >> 8) & 0x7;
    uint16_t cntlid = (dw10 >> 16) & 0xffff;
    int nr = dw11 & 0xffff;

    trace_pci_nvme_virt_mngmt(nvme_cid(req), act, cntlid, rt ? "VI" : "VQ", nr);

    if (rt != NVME_VIRT_RES_QUEUE && rt != NVME_VIRT_RES_INTERRUPT) {
        return NVME_INVALID_RESOURCE_ID | NVME_DNR;
    }

    switch (act) {
    case NVME_VIRT_MNGMT_ACTION_SEC_ASSIGN:
        return nvme_assign_virt_res_to_sec(n, req, cntlid, rt, nr);
    case NVME_VIRT_MNGMT_ACTION_PRM_ALLOC:
        return nvme_assign_virt_res_to_prim(n, req, cntlid, rt, nr);
    case NVME_VIRT_MNGMT_ACTION_SEC_ONLINE:
        return nvme_virt_set_state(n, cntlid, true);
    case NVME_VIRT_MNGMT_ACTION_SEC_OFFLINE:
        return nvme_virt_set_state(n, cntlid, false);
    default:
        return NVME_INVALID_FIELD | NVME_DNR;
    }
}

uint16_t nvme_directive_receive(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeNamespace *ns;
    uint32_t dw10 = le32_to_cpu(req->cmd.cdw10);
    uint32_t dw11 = le32_to_cpu(req->cmd.cdw11);
    uint32_t nsid = le32_to_cpu(req->cmd.nsid);
    uint8_t doper, dtype;
    uint32_t numd, trans_len;
    NvmeDirectiveIdentify id = {
        .supported = 1 << NVME_DIRECTIVE_IDENTIFY,
        .enabled = 1 << NVME_DIRECTIVE_IDENTIFY,
    };

    numd = dw10 + 1;
    doper = dw11 & 0xff;
    dtype = (dw11 >> 8) & 0xff;

    trans_len = MIN(sizeof(NvmeDirectiveIdentify), numd << 2);

    if (nsid == NVME_NSID_BROADCAST || dtype != NVME_DIRECTIVE_IDENTIFY ||
        doper != NVME_DIRECTIVE_RETURN_PARAMS) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    ns = nvme_ns(n, nsid);
    if (!ns) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    switch (dtype) {
    case NVME_DIRECTIVE_IDENTIFY:
        switch (doper) {
        case NVME_DIRECTIVE_RETURN_PARAMS:
            if (ns->endgrp && ns->endgrp->fdp.enabled) {
                id.supported |= 1 << NVME_DIRECTIVE_DATA_PLACEMENT;
                id.enabled |= 1 << NVME_DIRECTIVE_DATA_PLACEMENT;
                id.persistent |= 1 << NVME_DIRECTIVE_DATA_PLACEMENT;
            }

            return nvme_c2h(n, (uint8_t *)&id, trans_len, req);

        default:
            return NVME_INVALID_FIELD | NVME_DNR;
        }

    default:
        return NVME_INVALID_FIELD;
    }
}

uint16_t nvme_dbbuf_config(NvmeCtrl *n, NvmeRequest *req)
{
    PCIDevice *pci = PCI_DEVICE(n);
    uint64_t dbs_addr = le64_to_cpu(req->cmd.dptr.prp1);
    uint64_t eis_addr = le64_to_cpu(req->cmd.dptr.prp2);
    int i;

    /* Address should be page aligned */
    if (dbs_addr & (n->page_size - 1) || eis_addr & (n->page_size - 1)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    /* Save shadow buffer base addr for use during queue creation */
    n->dbbuf_dbs = dbs_addr;
    n->dbbuf_eis = eis_addr;
    n->dbbuf_enabled = true;

    for (i = 0; i < n->params.max_ioqpairs + 1; i++) {
        NvmeSQueue *sq = n->sq[i];
        NvmeCQueue *cq = n->cq[i];

        if (sq) {
            /*
             * CAP.DSTRD is 0, so offset of ith sq db_addr is (i<<3)
             * nvme_process_db() uses this hard-coded way to calculate
             * doorbell offsets. Be consistent with that here.
             */
            sq->db_addr = dbs_addr + (i << 3);
            sq->ei_addr = eis_addr + (i << 3);
            stl_le_pci_dma(pci, sq->db_addr, sq->tail, MEMTXATTRS_UNSPECIFIED);

            if (n->params.ioeventfd && sq->sqid != 0) {
                if (!nvme_init_sq_ioeventfd(sq)) {
                    sq->ioeventfd_enabled = true;
                }
            }
        }

        if (cq) {
            /* CAP.DSTRD is 0, so offset of ith cq db_addr is (i<<3)+(1<<2) */
            cq->db_addr = dbs_addr + (i << 3) + (1 << 2);
            cq->ei_addr = eis_addr + (i << 3) + (1 << 2);
            stl_le_pci_dma(pci, cq->db_addr, cq->head, MEMTXATTRS_UNSPECIFIED);

            if (n->params.ioeventfd && cq->cqid != 0) {
                if (!nvme_init_cq_ioeventfd(cq)) {
                    cq->ioeventfd_enabled = true;
                }
            }
        }
    }

    trace_pci_nvme_dbbuf_config(dbs_addr, eis_addr);

    return NVME_SUCCESS;
}

typedef struct NvmeFormatAIOCB {
    BlockAIOCB common;
    BlockAIOCB *aiocb;
    NvmeRequest *req;
    int ret;

    NvmeNamespace *ns;
    uint32_t nsid;
    bool broadcast;
    int64_t offset;

    uint8_t lbaf;
    uint8_t mset;
    uint8_t pi;
    uint8_t pil;
} NvmeFormatAIOCB;

static void nvme_format_cancel(BlockAIOCB *aiocb)
{
    NvmeFormatAIOCB *iocb = container_of(aiocb, NvmeFormatAIOCB, common);

    iocb->ret = -ECANCELED;

    if (iocb->aiocb) {
        blk_aio_cancel_async(iocb->aiocb);
        iocb->aiocb = NULL;
    }
}

static const AIOCBInfo nvme_format_aiocb_info = {
    .aiocb_size = sizeof(NvmeFormatAIOCB),
    .cancel_async = nvme_format_cancel,
};

static void nvme_format_set(NvmeNamespace *ns, uint8_t lbaf, uint8_t mset,
                            uint8_t pi, uint8_t pil)
{
    uint8_t lbafl = lbaf & 0xf;
    uint8_t lbafu = lbaf >> 4;

    trace_pci_nvme_format_set(ns->params.nsid, lbaf, mset, pi, pil);

    ns->id_ns.dps = (pil << 3) | pi;
    ns->id_ns.flbas = (lbafu << 5) | (mset << 4) | lbafl;

    nvme_ns_init_format(ns);
}

static void nvme_do_format(NvmeFormatAIOCB *iocb);

static void nvme_format_ns_cb(void *opaque, int ret)
{
    NvmeFormatAIOCB *iocb = opaque;
    NvmeNamespace *ns = iocb->ns;
    int bytes;

    if (iocb->ret < 0) {
        goto done;
    } else if (ret < 0) {
        iocb->ret = ret;
        goto done;
    }

    assert(ns);

    if (iocb->offset < ns->size) {
        bytes = MIN(BDRV_REQUEST_MAX_BYTES, ns->size - iocb->offset);

        iocb->aiocb = blk_aio_pwrite_zeroes(ns->blkconf.blk, iocb->offset,
                                            bytes, BDRV_REQ_MAY_UNMAP,
                                            nvme_format_ns_cb, iocb);

        iocb->offset += bytes;
        return;
    }

    nvme_format_set(ns, iocb->lbaf, iocb->mset, iocb->pi, iocb->pil);
    ns->status = 0x0;
    iocb->ns = NULL;
    iocb->offset = 0;

done:
    nvme_do_format(iocb);
}

static uint16_t nvme_format_check(NvmeNamespace *ns, uint8_t lbaf, uint8_t pi)
{
    if (ns->params.zoned) {
        return NVME_INVALID_FORMAT | NVME_DNR;
    }

    if (lbaf > ns->id_ns.nlbaf) {
        return NVME_INVALID_FORMAT | NVME_DNR;
    }

    if (pi && (ns->id_ns.lbaf[lbaf].ms < nvme_pi_tuple_size(ns))) {
        return NVME_INVALID_FORMAT | NVME_DNR;
    }

    if (pi && pi > NVME_ID_NS_DPS_TYPE_3) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    return NVME_SUCCESS;
}

static void nvme_do_format(NvmeFormatAIOCB *iocb)
{
    NvmeRequest *req = iocb->req;
    NvmeCtrl *n = nvme_ctrl(req);
    uint32_t dw10 = le32_to_cpu(req->cmd.cdw10);
    uint8_t lbaf = dw10 & 0xf;
    uint8_t pi = (dw10 >> 5) & 0x7;
    uint16_t status;
    int i;

    if (iocb->ret < 0) {
        goto done;
    }

    if (iocb->broadcast) {
        for (i = iocb->nsid + 1; i <= NVME_MAX_NAMESPACES; i++) {
            iocb->ns = nvme_ns(n, i);
            if (iocb->ns) {
                iocb->nsid = i;
                break;
            }
        }
    }

    if (!iocb->ns) {
        goto done;
    }

    status = nvme_format_check(iocb->ns, lbaf, pi);
    if (status) {
        req->status = status;
        goto done;
    }

    iocb->ns->status = NVME_FORMAT_IN_PROGRESS;
    nvme_format_ns_cb(iocb, 0);
    return;

done:
    iocb->common.cb(iocb->common.opaque, iocb->ret);
    qemu_aio_unref(iocb);
}

uint16_t nvme_format(NvmeCtrl *n, NvmeRequest *req)
{
    NvmeFormatAIOCB *iocb;
    uint32_t nsid = le32_to_cpu(req->cmd.nsid);
    uint32_t dw10 = le32_to_cpu(req->cmd.cdw10);
    uint8_t lbaf = dw10 & 0xf;
    uint8_t mset = (dw10 >> 4) & 0x1;
    uint8_t pi = (dw10 >> 5) & 0x7;
    uint8_t pil = (dw10 >> 8) & 0x1;
    uint8_t lbafu = (dw10 >> 12) & 0x3;
    uint16_t status;

    iocb = qemu_aio_get(&nvme_format_aiocb_info, NULL, nvme_misc_cb, req);

    iocb->req = req;
    iocb->ret = 0;
    iocb->ns = NULL;
    iocb->nsid = 0;
    iocb->lbaf = lbaf;
    iocb->mset = mset;
    iocb->pi = pi;
    iocb->pil = pil;
    iocb->broadcast = (nsid == NVME_NSID_BROADCAST);
    iocb->offset = 0;

    if (n->features.hbs.lbafee) {
        iocb->lbaf |= lbafu << 4;
    }

    if (!iocb->broadcast) {
        if (!nvme_nsid_valid(n, nsid)) {
            status = NVME_INVALID_NSID | NVME_DNR;
            goto out;
        }

        iocb->ns = nvme_ns(n, nsid);
        if (!iocb->ns) {
            status = NVME_INVALID_FIELD | NVME_DNR;
            goto out;
        }
    }

    req->aiocb = &iocb->common;
    nvme_do_format(iocb);

    return NVME_NO_COMPLETE;

out:
    qemu_aio_unref(iocb);

    return status;
}

static uint16_t nvme_sec_prot_spdm_send(NvmeCtrl *n, NvmeRequest *req)
{
    StorageSpdmTransportHeader hdr = {0};
    g_autofree uint8_t *sec_buf = NULL;
    uint32_t transfer_len = le32_to_cpu(req->cmd.cdw11);
    uint32_t transport_transfer_len = transfer_len;
    uint32_t dw10 = le32_to_cpu(req->cmd.cdw10);
    uint32_t recvd;
    uint16_t nvme_cmd_status, ret;
    uint8_t secp = extract32(dw10, 24, 8);
    uint16_t spsp = extract32(dw10, 8, 16);
    bool spdm_res;

    if (transport_transfer_len > UINT32_MAX - sizeof(hdr)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    transport_transfer_len += sizeof(hdr);
    if (transport_transfer_len > SPDM_SOCKET_MAX_MESSAGE_BUFFER_SIZE) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    ret = nvme_check_mdts(n, transport_transfer_len);
    if (ret != NVME_SUCCESS) {
        return ret;
    }

    /* Generate the NVMe transport header */
    hdr.security_protocol = secp;
    hdr.security_protocol_specific = cpu_to_le16(spsp);
    hdr.length = cpu_to_le32(transfer_len);

    sec_buf = g_try_malloc0(transport_transfer_len);
    if (!sec_buf) {
        return NVME_INTERNAL_DEV_ERROR;
    }

    /* Attach the transport header */
    memcpy(sec_buf, &hdr, sizeof(hdr));
    ret = nvme_h2c(n, sec_buf + sizeof(hdr), transfer_len, req);
    if (ret) {
        return ret;
    }

    spdm_res = spdm_socket_send(n->spdm_socket, SPDM_SOCKET_STORAGE_CMD_IF_SEND,
                                SPDM_SOCKET_TRANSPORT_TYPE_NVME, sec_buf,
                                transport_transfer_len);
    if (!spdm_res) {
        return NVME_DATA_TRAS_ERROR | NVME_DNR;
    }

    /* The responder shall ack with message status */
    recvd = spdm_socket_receive(n->spdm_socket, SPDM_SOCKET_TRANSPORT_TYPE_NVME,
                                &nvme_cmd_status,
                                SPDM_SOCKET_MAX_MSG_STATUS_LEN);

    nvme_cmd_status = be16_to_cpu(nvme_cmd_status);

    if (recvd < SPDM_SOCKET_MAX_MSG_STATUS_LEN) {
        return NVME_DATA_TRAS_ERROR | NVME_DNR;
    }

    return nvme_cmd_status;
}

/* From host to controller */
uint16_t nvme_security_send(NvmeCtrl *n, NvmeRequest *req)
{
    uint32_t dw10 = le32_to_cpu(req->cmd.cdw10);
    uint8_t secp = extract32(dw10, 24, 8);

    switch (secp) {
    case NVME_SEC_PROT_DMTF_SPDM:
        if (n->spdm_socket < 0) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }
        return nvme_sec_prot_spdm_send(n, req);
    default:
        /* Unsupported Security Protocol Type */
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    return NVME_INVALID_FIELD | NVME_DNR;
}

static uint16_t nvme_get_sec_prot_info(NvmeCtrl *n, NvmeRequest *req)
{
    uint32_t alloc_len = le32_to_cpu(req->cmd.cdw11);
    uint8_t resp[10] = {
        /* Support Security Protol List Length */
        [6] = 0, /* MSB */
        [7] = 2, /* LSB */
        /* Support Security Protocol List */
        [8] = SFSC_SECURITY_PROT_INFO,
        [9] = 0,
    };

    if (n->spdm_socket >= 0) {
        resp[9] = NVME_SEC_PROT_DMTF_SPDM;
    }

    if (alloc_len < 10) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    return nvme_c2h(n, resp, sizeof(resp), req);
}

static uint16_t nvme_sec_prot_spdm_receive(NvmeCtrl *n, NvmeRequest *req)
{
    StorageSpdmTransportHeader hdr;
    g_autofree uint8_t *rsp_spdm_buf = NULL;
    uint32_t dw10 = le32_to_cpu(req->cmd.cdw10);
    uint32_t alloc_len = le32_to_cpu(req->cmd.cdw11);
    uint32_t recvd, spdm_res;
    uint16_t nvme_cmd_status, ret;
    uint8_t secp = extract32(dw10, 24, 8);
    uint8_t spsp = extract32(dw10, 8, 16);
    if (!alloc_len) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    /* Generate the NVMe transport header */
    hdr = (StorageSpdmTransportHeader) {
        .security_protocol = secp,
        .security_protocol_specific = cpu_to_le16(spsp),
        .length = cpu_to_le32(alloc_len),
    };

    /* Forward if_recv to the SPDM Server with SPSP0 */
    spdm_res = spdm_socket_send(n->spdm_socket, SPDM_SOCKET_STORAGE_CMD_IF_RECV,
                                SPDM_SOCKET_TRANSPORT_TYPE_NVME,
                                &hdr, sizeof(hdr));
    if (!spdm_res) {
        return NVME_DATA_TRAS_ERROR | NVME_DNR;
    }

    /* The responder shall ack with message status */
    recvd = spdm_socket_receive(n->spdm_socket, SPDM_SOCKET_TRANSPORT_TYPE_NVME,
                                &nvme_cmd_status,
                                SPDM_SOCKET_MAX_MSG_STATUS_LEN);
    if (recvd < SPDM_SOCKET_MAX_MSG_STATUS_LEN) {
        return NVME_DATA_TRAS_ERROR | NVME_DNR;
    }

    nvme_cmd_status = be16_to_cpu(nvme_cmd_status);
    /* An error here implies the prior if_recv from requester was spurious */
    if (nvme_cmd_status != NVME_SUCCESS) {
        return nvme_cmd_status;
    }

    /* Clear to start receiving data from the server */
    rsp_spdm_buf = g_try_malloc0(alloc_len);
    if (!rsp_spdm_buf) {
        return NVME_INTERNAL_DEV_ERROR;
    }

    recvd = spdm_socket_receive(n->spdm_socket,
                                SPDM_SOCKET_TRANSPORT_TYPE_NVME,
                                rsp_spdm_buf, alloc_len);
    if (!recvd) {
        return NVME_DATA_TRAS_ERROR | NVME_DNR;
    }

    ret = nvme_c2h(n, rsp_spdm_buf, MIN(recvd, alloc_len), req);
    if (ret) {
        return ret;
    }

    return NVME_SUCCESS;
}

/* From controller to host */
uint16_t nvme_security_receive(NvmeCtrl *n, NvmeRequest *req)
{
    uint32_t dw10 = le32_to_cpu(req->cmd.cdw10);
    uint16_t spsp = extract32(dw10, 8, 16);
    uint8_t secp = extract32(dw10, 24, 8);

    switch (secp) {
    case SFSC_SECURITY_PROT_INFO:
        switch (spsp) {
        case 0:
            /* Supported security protocol list */
            return nvme_get_sec_prot_info(n, req);
        case 1:
            /* Certificate data */
            /* fallthrough */
        default:
            return NVME_INVALID_FIELD | NVME_DNR;
        }
    case NVME_SEC_PROT_DMTF_SPDM:
        if (n->spdm_socket < 0) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }
        return nvme_sec_prot_spdm_receive(n, req);
    default:
        return NVME_INVALID_FIELD | NVME_DNR;
    }
}

void nvme_cse_acs_ioctrl23_m(NvmeCmdSet *tbl)
{
    /* enable operations mandatory for a I/O controller
     * compliant with the v2.3 base specification */
    tbl->cmds[NVME_ADM_CMD_DELETE_SQ] = (NvmeCmdDef) {
        .cse = NVME_CMD_EFF_CSUPP,
        .handle = nvme_del_sq
    };
    tbl->cmds[NVME_ADM_CMD_CREATE_SQ] = (NvmeCmdDef) {
        .cse = NVME_CMD_EFF_CSUPP,
        .handle = nvme_create_sq,
    };
    tbl->cmds[NVME_ADM_CMD_GET_LOG_PAGE] = (NvmeCmdDef) {
        .cse = NVME_CMD_EFF_CSUPP,
        .handle = nvme_get_log,
    };
    tbl->cmds[NVME_ADM_CMD_DELETE_CQ] = (NvmeCmdDef) {
        .cse = NVME_CMD_EFF_CSUPP,
        .handle = nvme_del_cq,
    };
    tbl->cmds[NVME_ADM_CMD_CREATE_CQ] = (NvmeCmdDef) {
        .cse = NVME_CMD_EFF_CSUPP,
        .handle = nvme_create_cq,
    };
    tbl->cmds[NVME_ADM_CMD_IDENTIFY] = (NvmeCmdDef) {
        .cse = NVME_CMD_EFF_CSUPP,
        .handle = nvme_identify,
    };
    tbl->cmds[NVME_ADM_CMD_ABORT] = (NvmeCmdDef) {
        .cse = NVME_CMD_EFF_CSUPP,
        .handle = nvme_abort,
    };
    tbl->cmds[NVME_ADM_CMD_SET_FEATURES] = (NvmeCmdDef) {
        .cse = NVME_CMD_EFF_CSUPP,
        .handle = nvme_set_feature,
    };
    tbl->cmds[NVME_ADM_CMD_GET_FEATURES] = (NvmeCmdDef) {
        .cse = NVME_CMD_EFF_CSUPP,
        .handle = nvme_get_feature,
    };
    tbl->cmds[NVME_ADM_CMD_ASYNC_EV_REQ] = (NvmeCmdDef) {
        .cse = NVME_CMD_EFF_CSUPP,
        .handle = nvme_aer,
    };
}

void nvme_cse_acs_defaults(NvmeCmdSet *tbl)
{
    /* TODO: remove static nvme_cse_acs_default when done */
    /* TODO: *consider* if we need n->ops entries for any of this (I think not) */
    nvme_cse_acs_ioctrl23_m(tbl);

    tbl->cmds[NVME_ADM_CMD_NS_ATTACHMENT] = (NvmeCmdDef) {
        .cse = NVME_CMD_EFF_CSUPP | NVME_CMD_EFF_NIC |
               NVME_CMD_EFF_CCC,
        .handle = nvme_ns_attachment,
    };
    tbl->cmds[NVME_ADM_CMD_DIRECTIVE_SEND] = (NvmeCmdDef) {
        .cse = NVME_CMD_EFF_CSUPP,
        .handle = nvme_directive_send,
    };
    tbl->cmds[NVME_ADM_CMD_VIRT_MNGMT] = (NvmeCmdDef) {
        .cse = NVME_CMD_EFF_CSUPP,
        .handle = nvme_virt_mngmt,
    };
    tbl->cmds[NVME_ADM_CMD_DIRECTIVE_RECV] = (NvmeCmdDef) {
        .cse = NVME_CMD_EFF_CSUPP,
        .handle = nvme_directive_receive,
    };
    tbl->cmds[NVME_ADM_CMD_FORMAT_NVM] = (NvmeCmdDef) {
        .cse = NVME_CMD_EFF_CSUPP | NVME_CMD_EFF_LBCC,
        .handle = nvme_format,
    };
    tbl->cmds[NVME_ADM_CMD_SECURITY_SEND] = (NvmeCmdDef) {
        .cse = NVME_CMD_EFF_CSUPP,
        .handle = nvme_security_send,
    };
    tbl->cmds[NVME_ADM_CMD_SECURITY_RECV] = (NvmeCmdDef) {
        .cse = NVME_CMD_EFF_CSUPP,
        .handle = nvme_security_receive,
    };
}
