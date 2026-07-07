#ifndef HW_NVME_IDENTIFY_H
#define HW_NVME_IDENTIFY_H

#include "qemu/osdep.h"

typedef struct NvmeCtrl NvmeCtrl;
typedef struct NvmeRequest NvmeRequest;

uint16_t nvme_rpt_empty_id_struct(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_identify_ctrl(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_identify_ctrl_csi(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_identify_ns(NvmeCtrl *n, NvmeRequest *req, bool active);
uint16_t nvme_identify_ctrl_list(NvmeCtrl *n, NvmeRequest *req,
                                 bool attached);
uint16_t nvme_identify_pri_ctrl_cap(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_identify_sec_ctrl_list(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_identify_ns_ind(NvmeCtrl *n, NvmeRequest *req, bool alloc);
uint16_t nvme_identify_ns_csi(NvmeCtrl *n, NvmeRequest *req,
                              bool active);
uint16_t nvme_identify_nslist(NvmeCtrl *n, NvmeRequest *req,
                              bool active);
uint16_t nvme_identify_nslist_csi(NvmeCtrl *n, NvmeRequest *req,
                                  bool active);
uint16_t nvme_endurance_group_list(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_identify_ns_descr_list(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_identify_cmd_set(NvmeCtrl *n, NvmeRequest *req);

uint16_t nvme_identify(NvmeCtrl *n, NvmeRequest *req);
#endif /* HW_NVME_IDENTIFY_H */
