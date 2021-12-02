/*
 * QEMU NVMe-MI Controller
 *
 * Copyright (c) 2021, Samsung Electronics co Ltd.
 *
 * Written by Padmakar Kalghatgi <p.kalghatgi@samsung.com>
 *            Arun Kumar Agasar <arun.kka@samsung.com>
 *            Saurav Kumar <saurav.29@partner.samsung.com>
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

/**
 * Reference Specs: http://www.nvmexpress.org,
 *
 *
 * Usage
 * -----
 * The nvme-mi device has to be used along with nvme device only
 *
 * Add options:
 *    -device  nvme-mi,nvme=<nvme id>,address=0x15",
 *
 */

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-core.h"
#include "hw/block/block.h"
#include "hw/pci/msix.h"
#include "include/block/nvme.h"
#include "nvme.h"
#include "nvme-mi.h"
#include "qemu/crc32c.h"
#include "hw/i2c/smbus_master.h"

#define NVME_TEMPERATURE 0x143
#define NVME_TEMPERATURE_WARNING 0x157
#define NVME_TEMPERATURE_CRITICAL 0x175

static uint8_t nvme_mi_gen_pec(uint8_t *data, size_t len)
{
    uint8_t pec = 0xff;
    size_t i, j;
    for (i = 0; i < len; i++) {
        pec ^= data[i];
        for (j = 0; j < 8; j++) {
            if ((pec & 0x80) != 0) {
                pec = (uint8_t)((pec << 1) ^ 0x31);
            } else {
                pec <<= 1;
            }
        }
    }
    return pec;
}
static void nvme_mi_send_resp(NvmeMiCtrl *ctrl_mi, uint8_t *resp, uint32_t size)
{
    uint32_t crc_value = crc32c(0xFFFFFFFF, resp, size);
    uint32_t offset = 0;
    uint32_t som = 1;
    uint32_t eom = 0;
    uint32_t pktseq = 0;
    uint32_t mtus = ctrl_mi->mctp_unit_size;
    size += sizeof(crc_value);
    while (size > 0) {
        size_t sizesent = MIN(size, mtus);
        size -= sizesent;
        eom = size > 0 ? 0 : 1;
        g_autofree uint8_t *buf = (uint8_t *)g_malloc0(sizesent + 8);
        buf[2] = sizesent + 5;
        buf[7] = (som << 7) | (eom << 6) | (pktseq << 5);
        som = 0;
        memcpy(buf + 8, resp + offset, sizesent);
        uint8_t pec = nvme_mi_gen_pec(resp + offset, sizesent);
        buf[sizesent + 8] = pec;
        offset += sizesent;
        if (size <= 0) {
            memcpy(buf + sizesent + NVME_MI_SMBUS_HEADER_AND_PEC - sizeof(crc_value),
                   &crc_value, sizeof(crc_value));
        }
        memcpy(ctrl_mi->misendrecv.state.sendrecvbuf + ctrl_mi->misendrecv.total_len,
               buf, sizesent + NVME_MI_SMBUS_HEADER_AND_PEC);
        ctrl_mi->misendrecv.total_len += sizesent + NVME_MI_SMBUS_HEADER_AND_PEC;

    }


}

static void nvme_mi_resp_hdr_init(NvmeMiResponse *resp, int NvmeMiType)
{
    resp->msg_header.msgtype = 4;
    resp->msg_header.ic = 1;
    resp->msg_header.csi = 0;
    resp->msg_header.reserved = 0;
    resp->msg_header.nmimt = NvmeMiType;
    resp->msg_header.ror = 1;
    resp->msg_header.reserved1 = 0;
}
static void nvme_mi_nvm_subsys_ds(NvmeMiCtrl *ctrl_mi, NvmeMiRequest *req)
{
    NvmeMiResponse resp;
    NvmMiSubsysInfoDs ds;
    uint32_t total_size = sizeof(resp) + sizeof(ds);
    g_autofree uint8_t *resp_message = g_new(uint8_t, total_size);
    ds.nump = 1;
    ds.mjr = (ctrl_mi->n->bar.vs & 0xFF0000) >> 16;
    ds.mnr = (ctrl_mi->n->bar.vs & 0xFF00) >> 8;

    nvme_mi_resp_hdr_init(&resp , NVME_MI_CMD);
    resp.status = SUCCESS;
    resp.mgmt_resp = sizeof(ds);
    memcpy(resp_message, &resp, sizeof(resp));
    memcpy(resp_message + sizeof(resp), &ds, sizeof(ds));

    nvme_mi_send_resp(ctrl_mi, resp_message, total_size);
}

static void nvme_mi_opt_supp_cmd_list(NvmeMiCtrl *ctrl_mi, NvmeMiRequest *req)
{
    NvmeMiResponse resp;
    uint32_t offset = 0, size = 0, total_size = 0;
    uint16_t mi_opt_cmd_cnt, admin_mi_opt_cmd_cnt, total_commands;
    g_autofree uint8_t *resp_message = NULL;
    g_autofree uint8_t *cmd_supp_list = NULL;
    nvme_mi_resp_hdr_init(&resp , NVME_MI_CMD);
    resp.status = SUCCESS;

    mi_opt_cmd_cnt = sizeof(NvmeMiCmdOptSupList) /
                              sizeof(uint32_t);
    admin_mi_opt_cmd_cnt = sizeof(NvmeMiAdminCmdOptSupList) /
                                    sizeof(uint32_t);

    total_commands = mi_opt_cmd_cnt + admin_mi_opt_cmd_cnt;
    size = 2 * (total_commands + 1);

    cmd_supp_list = (uint8_t *)g_malloc0(size);

    memcpy(cmd_supp_list, &total_commands, sizeof(uint16_t));
    offset += sizeof(uint16_t);
    /* TODO: remove conditional check when struct has entries */
    if (sizeof(NvmeMiCmdOptSupList)) {
        for (uint32_t i = 0; i < mi_opt_cmd_cnt; i++) {
            memcpy(cmd_supp_list + offset, &NvmeMiCmdOptSupList[i],
                   sizeof(uint8_t));
            cmd_supp_list[offset + 1] = 1;
            offset += 2;
        }
    }

    /* TODO: remove conditional check when struct has entries */
    if (sizeof(NvmeMiAdminCmdOptSupList)) {
        for (uint32_t i = 0; i < admin_mi_opt_cmd_cnt; i++) {
            memcpy(cmd_supp_list + offset, &NvmeMiAdminCmdOptSupList[i],
                   sizeof(uint8_t));
            cmd_supp_list[offset + 1] = 1;
            offset += 2;
        }
    }

    resp.mgmt_resp = size;
    total_size = sizeof(resp) + size;
    resp_message = (uint8_t *) g_malloc(total_size);
    memcpy(resp_message, &resp, sizeof(resp));
    memcpy(resp_message + sizeof(resp), cmd_supp_list, size);

    nvme_mi_send_resp(ctrl_mi, resp_message, total_size);
}

static void nvme_mi_controller_health_ds(NvmeMiCtrl *ctrl_mi,
                                         NvmeMiRequest *req)
{
    uint32_t dword0 = req->dword0;
    uint32_t dword1 = req->dword1;
    uint32_t maxrent = (dword0 >> 16) & 0xFF;
    uint32_t reportall = (dword0 >> 31) & 0x1;
    uint32_t incvf = (dword0 >> 26) & 0x1;
    uint32_t incpf = (dword0 >> 25) & 0x1;
    uint32_t incf = (dword0 >> 24) & 0x1;
    g_autofree uint8_t *resp_buf = NULL;

    NvmeMiResponse resp;
    nvme_mi_resp_hdr_init(&resp , NVME_MI_CMD);

    if (maxrent > 255 || (reportall == 0) || incvf || incpf || (incf == 0)) {
        resp.status = INVALID_PARAMETER;
        return nvme_mi_send_resp(ctrl_mi, (uint8_t *)&resp, sizeof(resp));
    }
    NvmeMiCtrlHealthDs nvme_mi_chds;
    if (dword1 & 0x1) {
        nvme_mi_chds.csts.rdy = ctrl_mi->n->bar.csts & 0x1;
        nvme_mi_chds.csts.cfs |= ctrl_mi->n->bar.csts & 0x2;
        nvme_mi_chds.csts.shst |= ctrl_mi->n->bar.csts & 0xa;
        nvme_mi_chds.csts.nssro |= ctrl_mi->n->bar.csts & 0x10;
        nvme_mi_chds.csts.en |= ctrl_mi->n->bar.cc & 0x1 << 5;
    }
    if (dword1 & 0x2) {
        nvme_mi_chds.ctemp = ctrl_mi->n->temperature;
    }
    if (((ctrl_mi->n->temperature >= ctrl_mi->n->features.temp_thresh_hi) ||
        (ctrl_mi->n->temperature <= ctrl_mi->n->features.temp_thresh_low)) &&
         (dword1 & 0x2)) {
        nvme_mi_chds.cwarn.temp_above_or_under_thresh = 0x1;
    }
    resp_buf = (uint8_t *)g_malloc(sizeof(resp) +
                                   sizeof(NvmeMiCtrlHealthDs));
    resp.mgmt_resp = 1 << 0x10;
    memcpy(resp_buf, &resp, sizeof(resp));
    memcpy(resp_buf + sizeof(resp), &nvme_mi_chds, sizeof(nvme_mi_chds));
    nvme_mi_send_resp(ctrl_mi, resp_buf, sizeof(resp) + sizeof(NvmeMiCtrlHealthDs));
}

static void nvme_mi_read_nvme_mi_ds(NvmeMiCtrl *ctrl_mi, NvmeMiRequest *req)
{
    ReadNvmeMiDs ds;
    int dtyp;
    ds.cntrlid = req->dword0 & 0xFFFF;
    ds.portlid = (req->dword0 & 0xFF0000) >> 16;
    ds.dtyp = (req->dword0 & ~0xFF) >> 24;
    dtyp = ds.dtyp;
    switch (dtyp) {
    case NVM_SUBSYSTEM_INFORMATION:
        nvme_mi_nvm_subsys_ds(ctrl_mi, req);
        break;
    case OPT_SUPP_CMD_LIST:
        nvme_mi_opt_supp_cmd_list(ctrl_mi, req);
        break;
    }
}

static void nvme_mi_configuration_get(NvmeMiCtrl *ctrl_mi, NvmeMiRequest *req)
{
    uint8_t config_identifier = (req->dword0 & 0xFF);
    NvmeMiResponse resp;
    uint32_t total_size = sizeof(resp);
    g_autofree uint8_t *resp_message = g_new(uint8_t, total_size);
    switch (config_identifier) {
    case SMBUS_I2C_FREQ: {
       nvme_mi_resp_hdr_init(&resp, NVME_MI_CMD);
       resp.status = SUCCESS;
       resp.mgmt_resp = ctrl_mi->smbus_freq;
       memcpy(resp_message, &resp, sizeof(resp));

       nvme_mi_send_resp(ctrl_mi, resp_message, total_size);
    }
    break;
    case MCTP_TRANS_UNIT_SIZE: {
        nvme_mi_resp_hdr_init(&resp, NVME_MI_CMD);
        resp.status = SUCCESS;
        resp.mgmt_resp = ctrl_mi->mctp_unit_size;
        memcpy(resp_message, &resp, sizeof(resp));

        nvme_mi_send_resp(ctrl_mi, resp_message, total_size);
    }
    break;
    }
}

static void nvme_mi_configuration_set(NvmeMiCtrl *ctrl_mi, NvmeMiRequest *req)
{
    uint8_t config_identifier = (req->dword0 & 0xFF);
    NvmeMiResponse resp;
    uint32_t total_size = sizeof(resp);
    g_autofree uint8_t *resp_message = g_new(uint8_t, total_size);
    switch (config_identifier) {
    case SMBUS_I2C_FREQ: {
        nvme_mi_resp_hdr_init(&resp, NVME_MI_CMD);
        resp.status = SUCCESS;
        resp.mgmt_resp = 0;
        ctrl_mi->smbus_freq = (req->dword0 & 0xF00) >> 8;
        memcpy(resp_message, &resp, sizeof(resp));

        nvme_mi_send_resp(ctrl_mi, resp_message, total_size);
    }
    break;
    case MCTP_TRANS_UNIT_SIZE: {
        nvme_mi_resp_hdr_init(&resp, NVME_MI_CMD);
        resp.status = SUCCESS;
        ctrl_mi->mctp_unit_size = (req->dword1 & 0xFFFF);
        memcpy(resp_message, &resp, sizeof(resp));

        nvme_mi_send_resp(ctrl_mi, resp_message, total_size);
    }
    break;
    default:
        nvme_mi_resp_hdr_init(&resp, NVME_MI_CMD);
        resp.status = INVALID_PARAMETER;
        memcpy(resp_message, &resp, sizeof(resp));
        nvme_mi_send_resp(ctrl_mi, resp_message, total_size);
    }

}

static void nvme_mi_vpd_read(NvmeMiCtrl *ctrl_mi, NvmeMiRequest *req)
{
    uint16_t dofst = (req->dword0 & 0xFFFF);
    uint16_t dlen = (req->dword1 & 0xFFFF);
    NvmeMiResponse resp;
    g_autofree uint8_t *resp_buf = NULL;
    nvme_mi_resp_hdr_init(&resp, NVME_MI_CMD);
    if ((dofst + dlen) > sizeof(NvmeMiVpdElements)) {
        resp.status = INVALID_PARAMETER;
        nvme_mi_send_resp(ctrl_mi, (uint8_t *)&resp, sizeof(resp));
    } else {
        resp.status = SUCCESS;
        resp_buf = (uint8_t *) g_malloc(dlen + sizeof(resp));
        memcpy(resp_buf, &resp, sizeof(resp));
        memcpy(resp_buf + sizeof(resp), &ctrl_mi->vpd_data + dofst, dlen);
        nvme_mi_send_resp(ctrl_mi, resp_buf, dlen + sizeof(resp));
    }
}
static void nvme_mi_vpd_write(NvmeMiCtrl *ctrl_mi,
                              NvmeMiRequest *req, uint8_t *buf)
{
    uint16_t dofst = (req->dword0 & 0xFFFF);
    uint16_t dlen = (req->dword1 & 0xFFFF);
    NvmeMiResponse resp;
    nvme_mi_resp_hdr_init(&resp, NVME_MI_CMD);
    if ((dofst + dlen) > sizeof(NvmeMiVpdElements)) {
        resp.status = INVALID_PARAMETER;
        nvme_mi_send_resp(ctrl_mi, (uint8_t *)&resp, sizeof(resp));
    } else {
        resp.status = SUCCESS;
        memcpy(&ctrl_mi->vpd_data + dofst, buf + 16 , dlen);
        nvme_mi_send_resp(ctrl_mi, (uint8_t *)&resp, sizeof(resp));
    }
}

static void nvme_mi_nvm_subsys_health_status_poll(NvmeMiCtrl *ctrl_mi,
                                                  NvmeMiRequest *req)
{
    NvmeMiResponse resp;
    NvmeMiNvmSubsysHspds nshds;
    NvmeCtrl *ctrl = NULL;
    g_autofree uint8_t *resp_buf = NULL;
    nvme_mi_resp_hdr_init(&resp, NVME_MI_CMD);
    for (uint32_t cntlid = 1; cntlid < ARRAY_SIZE(ctrl_mi->n->subsys->ctrls);
                  cntlid++) {

        ctrl = nvme_subsys_ctrl(ctrl_mi->n->subsys, cntlid);
        if (!ctrl) {
            continue;
        }

        if ((ctrl->bar.csts & 0x1) == 0x1) {
            nshds.ccs = 0x1;
        }
        if ((ctrl->bar.csts & 0x2) == 0x2) {
            nshds.ccs |= 0x2;
        }
        if ((ctrl->bar.csts & 0x10) == 0x10) {
            nshds.ccs |= 0x10;
        }
        if (find_first_bit(ctrl->changed_nsids, NVME_CHANGED_NSID_SIZE) !=
            NVME_CHANGED_NSID_SIZE) {
                nshds.ccs |= 0x40;
        }
        if ((ctrl->temperature >= ctrl->features.temp_thresh_hi) ||
           (ctrl->temperature <= ctrl->features.temp_thresh_low)) {
            nshds.ccs |= 0x200;
        }
    }


    resp_buf = (uint8_t *)g_malloc(sizeof(resp) + sizeof(nshds));
    memcpy(resp_buf, &resp, sizeof(resp));
    memcpy(resp_buf + sizeof(resp), &nshds, sizeof(nshds));
    nvme_mi_send_resp(ctrl_mi, resp_buf, sizeof(resp_buf));
}

static void nvme_mi_admin_identify_ns(NvmeMiCtrl *ctrl_mi,
                                      NvmeAdminMiRequest *req,
                                      uint32_t dofst, uint32_t dlen)
{
    NvmeIdNs *id_ns;
    uint32_t nsid = req->sqentry1;
    NvmeMiAdminResponse resp;
    NvmeNamespace *ns;
    g_autofree uint8_t *resp_buff = NULL;
    nvme_mi_resp_hdr_init((NvmeMiResponse *)&resp, NVME_ADM_CMD);
    resp.status = SUCCESS;
    ns = nvme_ns(ctrl_mi->n, nsid);
    if (!ns) {
        resp.cqdword0 = 0;
        resp.cqdword1 = 0;
        resp.cqdword3 = NVME_INVALID_NSID << 16;
            nvme_mi_send_resp(ctrl_mi, (uint8_t *)&resp, sizeof(NvmeMiAdminResponse));
        return ;
    }

    id_ns = &ns->id_ns;

    resp_buff = g_malloc0(sizeof(NvmeMiAdminResponse) + dlen);
    memcpy(resp_buff, &resp, sizeof(NvmeMiAdminResponse));
    memcpy(resp_buff + sizeof(NvmeMiAdminResponse), id_ns + dofst, dlen);

    nvme_mi_send_resp(ctrl_mi, (uint8_t *)resp_buff,
                      (sizeof(NvmeMiAdminResponse) + dlen));
}
static void nvme_mi_admin_identify_ctrl(NvmeMiCtrl *ctrl_mi,
                                        NvmeAdminMiRequest *req,
                                        uint32_t dofst, uint32_t dlen)
{
    NvmeMiAdminResponse resp;
    g_autofree uint8_t *resp_buff = NULL;
    nvme_mi_resp_hdr_init((NvmeMiResponse *)&resp, NVME_ADM_CMD);
    resp.status = SUCCESS;
    resp_buff = g_malloc0(sizeof(NvmeMiAdminResponse) + dlen);
    memcpy(resp_buff, &resp, sizeof(NvmeMiAdminResponse));
    memcpy(resp_buff + sizeof(NvmeMiAdminResponse), &ctrl_mi->n->id_ctrl + dofst, dlen);

    nvme_mi_send_resp(ctrl_mi, (uint8_t *)resp_buff,
                     (sizeof(NvmeMiAdminResponse) + dlen));
}
static void nvme_mi_admin_identify(NvmeMiCtrl *ctrl_mi, NvmeAdminMiRequest *req)
{
    uint32_t cns = req->sqentry10 & 0xFF;
    uint32_t cflags = req->cmdflags;
    uint32_t dofst = req->dataofst;
    uint32_t dlen = req->datalen;
    NvmeMiResponse resp;
    if (dofst + dlen > 4096) {
        nvme_mi_resp_hdr_init(&resp, true);
        resp.status = INVALID_PARAMETER;
        return nvme_mi_send_resp(ctrl_mi, (uint8_t *)&resp, sizeof(resp));
    }
    if ((cflags & 0x1) == 0) {
        dlen = 4096;
    }
    if (!(cflags & 0x2)) {
        dofst = 0;
    }
    switch (cns) {
    case 0x00:
        return nvme_mi_admin_identify_ns(ctrl_mi, req, dofst, dlen);
    case 0x1:
        return nvme_mi_admin_identify_ctrl(ctrl_mi, req, dofst, dlen);
    default:
    {
        NvmeMiAdminResponse _resp;
        nvme_mi_resp_hdr_init((NvmeMiResponse *)&_resp, NVME_ADM_CMD);
        _resp.status = SUCCESS;
        _resp.cqdword3 = NVME_INVALID_FIELD << 16;
        nvme_mi_send_resp(ctrl_mi, (uint8_t *)&_resp, sizeof(_resp));
    }
    }
}
static void nvme_mi_admin_error_info_log(NvmeMiCtrl *ctrl_mi,
                                         NvmeAdminMiRequest *req,
                                         uint32_t dofst, uint32_t dlen)
{
    NvmeMiAdminResponse resp;
    NvmeErrorLog errlog = { };
    g_autofree uint8_t *resp_buff = NULL;
    memset(&errlog, 0x0, sizeof(errlog));
    nvme_mi_resp_hdr_init((NvmeMiResponse *)&resp, NVME_ADM_CMD);
    resp.status = SUCCESS;
    resp_buff = g_malloc0(sizeof(NvmeMiAdminResponse) + dlen);
    memcpy(resp_buff, &resp, sizeof(NvmeMiAdminResponse));
    memcpy(resp_buff + sizeof(NvmeMiAdminResponse), &errlog + dofst, dlen);
    nvme_mi_send_resp(ctrl_mi, (uint8_t *)resp_buff,
                     (sizeof(NvmeMiAdminResponse) + dlen));
}

static void nvme_mi_admin_get_log_page(NvmeMiCtrl *ctrl_mi,
                                       NvmeAdminMiRequest *req)
{
    uint32_t lid = req->sqentry10;
    uint32_t cflags = req->cmdflags;
    uint32_t dofst = req->dataofst;
    uint32_t dlen = req->datalen;
    NvmeMiResponse resp;

    switch (lid) {
    case 0x00:
        if (dofst + dlen > sizeof(NvmeErrorLog)) {
            nvme_mi_resp_hdr_init(&resp, NVME_ADM_CMD);
            resp.status = INVALID_PARAMETER;
            return nvme_mi_send_resp(ctrl_mi, (uint8_t *)&resp, sizeof(resp));
        }
        if ((cflags & 0x1) == 0) {
            dlen = sizeof(NvmeErrorLog);
        }
        if (!(cflags & 0x2)) {
            dofst = 0;
        }
        if (dofst + dlen > sizeof(NvmeErrorLog)) {
            nvme_mi_resp_hdr_init(&resp, NVME_ADM_CMD);
            resp.status = INVALID_PARAMETER;
            return nvme_mi_send_resp(ctrl_mi, (uint8_t *)&resp, sizeof(resp));
        }

        return nvme_mi_admin_error_info_log(ctrl_mi, req, dofst, dlen);
    default:
    {
        NvmeMiAdminResponse _resp;
        nvme_mi_resp_hdr_init((NvmeMiResponse *)&_resp, NVME_ADM_CMD);
        _resp.status = SUCCESS;
        _resp.cqdword3 = NVME_INVALID_FIELD << 16;
        nvme_mi_send_resp(ctrl_mi, (uint8_t *)&_resp, sizeof(_resp));
    }
    }
}

static void nvme_mi_admin_get_features(NvmeMiCtrl *ctrl_mi,
                                       NvmeAdminMiRequest *req)
{
    uint32_t fid = req->sqentry10 & 0xFF;
    uint32_t dofst = req->dataofst;
    uint32_t dlen = req->datalen;
    NvmeMiResponse miresp;
    NvmeMiAdminResponse miadminresp;
    if (dofst || dlen) {

        nvme_mi_resp_hdr_init(&miresp, NVME_ADM_CMD);
        miresp.status = INVALID_PARAMETER;
        return nvme_mi_send_resp(ctrl_mi, (uint8_t *)&miresp, sizeof(miresp));
    }

    nvme_mi_resp_hdr_init((NvmeMiResponse *)&miadminresp, NVME_ADM_CMD);
    miadminresp.status = SUCCESS;

    switch (fid) {
    case NVME_TEMPERATURE_THRESHOLD:
        miadminresp.cqdword0 = 0;

        if (NVME_TEMP_TMPSEL(req->sqentry11) != NVME_TEMP_TMPSEL_COMPOSITE) {
            break;
        }

        if (NVME_TEMP_THSEL(req->sqentry11) == NVME_TEMP_THSEL_OVER) {
            miadminresp.cqdword0 = NVME_TEMPERATURE_WARNING;
        }
        break;
    case NVME_NUMBER_OF_QUEUES:
        miadminresp.cqdword0 = (ctrl_mi->n->params.max_ioqpairs - 1) |
                        ((ctrl_mi->n->params.max_ioqpairs - 1) << 16);
        break;
    default:
        miadminresp.cqdword3 = NVME_INVALID_FIELD << 16;
        break;
    }

    return nvme_mi_send_resp(ctrl_mi, (uint8_t *)&miadminresp, sizeof(miadminresp));
}

static void nvme_mi_admin_command(NvmeMiCtrl *ctrl_mi, void* req_arg)
{
    uint8_t *msg  = ((uint8_t *)req_arg);
    NvmeMiMessageHeader msghdr;
    memcpy(&msghdr, msg, sizeof(NvmeMiMessageHeader));
    if (msghdr.nmimt == 1) {
        NvmeMiRequest *req = (NvmeMiRequest *) (msg);
        switch (req->opc) {
        case READ_NVME_MI_DS:
            nvme_mi_read_nvme_mi_ds(ctrl_mi, req);
            break;
        case CHSP:
            nvme_mi_controller_health_ds(ctrl_mi, req);
            break;
        case NVM_SHSP:
            nvme_mi_nvm_subsys_health_status_poll(ctrl_mi, req);
            break;
        case CONFIGURATION_SET:
            nvme_mi_configuration_set(ctrl_mi, req);
            break;
        case CONFIGURATION_GET:
            nvme_mi_configuration_get(ctrl_mi, req);
            break;
        case VPD_READ:
            nvme_mi_vpd_read(ctrl_mi, req);
            break;
        case VPD_WRITE:
            nvme_mi_vpd_write(ctrl_mi, req, msg);
            break;
        default:
        {
            NvmeMiResponse resp;
            nvme_mi_resp_hdr_init(&resp, false);
            resp.status = INVALID_COMMAND_OPCODE;
            nvme_mi_send_resp(ctrl_mi, (uint8_t *)&resp, sizeof(resp));
            break;
        }
        }
    } else {
        NvmeAdminMiRequest *req = (NvmeAdminMiRequest *) (msg);
        switch  (req->opc) {
        case NVME_ADM_CMD_IDENTIFY:
            nvme_mi_admin_identify(ctrl_mi, req);
            break;
        case NVME_ADM_CMD_GET_LOG_PAGE:
            nvme_mi_admin_get_log_page(ctrl_mi, req);
            break;
        case NVME_ADM_CMD_GET_FEATURES:
            nvme_mi_admin_get_features(ctrl_mi, req);
            break;
        default:
        {
            NvmeMiResponse resp;
            nvme_mi_resp_hdr_init(&resp, true);
            resp.status = INVALID_COMMAND_OPCODE;
            nvme_mi_send_resp(ctrl_mi, (uint8_t *)&resp, sizeof(resp));
            break;
        }
        }
    }

    return;
}

static int nvme_mi_i2c_send(I2CSlave *s, uint8_t data)
{
    NvmeMiCtrl *mictrl = (NvmeMiCtrl *)s;
    NvmeMiSendRecvStruct *misendrecv = &mictrl->misendrecv;

    switch (misendrecv->state.pktpos) {
    case NVME_MI_BYTE_LENGTH_POS:
        misendrecv->state.pktlen = data + 1;
        break;
    case NVME_MI_HOST_SLAVE_ADDR_POS:
        misendrecv->hostslaveaddr = data >> 1;
        break;
    case NVME_MI_EOM_POS:
        misendrecv->eom = (data >> 6) & 1;
        break;
    }
    misendrecv->state.sendrecvbuf[++misendrecv->state.pktpos] = data;
    if (misendrecv->state.pktpos == misendrecv->state.pktlen + 3) {
        misendrecv->cmdbuffer = (uint8_t *)g_realloc(misendrecv->cmdbuffer,
                                                     misendrecv->offset +
                                                     misendrecv->state.pktlen - 5);
        memcpy(misendrecv->cmdbuffer + misendrecv->offset,
               misendrecv->state.sendrecvbuf + 8, misendrecv->state.pktlen - 5);

        misendrecv->offset += misendrecv->state.pktlen - 5;
        misendrecv->state.pktlen = 0;
        misendrecv->state.pktpos = 0;

        if (misendrecv->eom == 1) {
            misendrecv->total_len = 0;
            misendrecv->eom = 0;
            nvme_mi_admin_command(mictrl, misendrecv->cmdbuffer);
            misendrecv->offset = 0;
            i2c_end_transfer(mictrl->bus);
            for (int i = 0; i < misendrecv->total_len; i++) {
                smbus_send_byte(mictrl->bus, misendrecv->hostslaveaddr,
                                misendrecv->state.sendrecvbuf[i]);
            }
        }
    }
    return 0;
}

static void nvme_mi_realize(DeviceState *dev, Error **errp)
{
    NvmeMiCtrl *s = (NvmeMiCtrl *)(dev);
    s->bus = (I2CBus *)dev->parent_bus;
    s->smbus_freq = NVME_MI_DEF_SMBUS_FREQ;
    s->mctp_unit_size = NVME_MI_DEF_MCTP_TRANS_UNIT_SIZE;
}
static Property nvme_mi_props[] = {
     DEFINE_PROP_LINK("nvme", NvmeMiCtrl, n, TYPE_NVME, NvmeCtrl *),
};

static void nvme_mi_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = nvme_mi_realize;
    k->send = nvme_mi_i2c_send;

    device_class_set_props(dc, nvme_mi_props);
}

static const TypeInfo nvme_mi = {
    .name = TYPE_NVME_MI,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(NvmeMiCtrl),
    .class_init = nvme_mi_class_init,
};

static void register_types(void)
{
    type_register_static(&nvme_mi);
}

type_init(register_types);
