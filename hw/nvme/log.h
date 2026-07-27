#ifndef HW_NVME_LOG_H
#define HW_NVME_LOG_H
#include "qemu/osdep.h"

typedef struct NvmeCtrl NvmeCtrl;
typedef struct NvmeRequest NvmeRequest;
typedef struct NvmeLogRqCtx NvmeLogRqCtx;
typedef struct NvmeLogSet NvmeLogSet;

void nvme_clear_events(NvmeCtrl *n, uint8_t event_type);

uint16_t nvme_error_info(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx);
uint16_t nvme_smart_info(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx);
uint16_t nvme_fw_log_info(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx);

uint16_t nvme_log_ocp_extended_smart_info(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx);
uint16_t nvme_changed_nslist(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx);
uint16_t nvme_cmd_effects(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx);
uint16_t nvme_endgrp_info(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx);
uint16_t nvme_fdp_confs(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx);
uint16_t nvme_fdp_ruh_usage(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx);
uint16_t nvme_fdp_stats(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx);
uint16_t nvme_fdp_events(NvmeCtrl *n, NvmeRequest *req, NvmeLogRqCtx *ctx);

void nvme_init_ctrl_log_io_adm_14m(NvmeLogSet *ls);
void nvme_init_ctrl_log_io_adm_23m(NvmeLogSet *ls);
void nvme_init_ctrl_log_default(NvmeLogSet *ls);

uint16_t nvme_get_log(NvmeCtrl *n, NvmeRequest *req);
#endif /* HW_NVME_LOG_H */
