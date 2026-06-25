#ifndef HW_NVME_FEATURES_H
#define HW_NVME_FEATURES_H

#include "qemu/osdep.h"

typedef struct NvmeCtrl NvmeCtrl;
typedef struct NvmeRequest NvmeRequest;

bool nvme_feature_supported(NvmeCtrl *n, uint8_t fid);


uint16_t nvme_get_feature_timestamp(NvmeCtrl *n, NvmeRequest *req, bool defval);

uint16_t nvme_get_feature_arbitration(NvmeCtrl *n, NvmeRequest *req, bool defval);
uint16_t nvme_get_feature_temp_threshold(NvmeCtrl *n, NvmeRequest *req, bool defval);
uint16_t nvme_get_feature_errrec(NvmeCtrl *n, NvmeRequest *req, bool defval);
uint16_t nvme_get_feature_vwc(NvmeCtrl *n, NvmeRequest *req, bool defval);
uint16_t nvme_get_feature_numqs(NvmeCtrl *n, NvmeRequest *req, bool defval);
uint16_t nvme_get_feature_int_vec_conf(NvmeCtrl *n, NvmeRequest *req, bool defval);
uint16_t nvme_get_feature_write_atomicity(NvmeCtrl *n, NvmeRequest *req,
                                          bool defval);
uint16_t nvme_get_feature_aec(NvmeCtrl *n, NvmeRequest *req, bool defval);
uint16_t nvme_get_feature_hbs(NvmeCtrl *n, NvmeRequest *req, bool defval);
uint16_t nvme_get_feature_fdp(NvmeCtrl *n, NvmeRequest *req, bool defval);
uint16_t nvme_get_feature_fdp_events(NvmeCtrl *n, NvmeRequest *req, bool defval);
uint16_t nvme_get_feature_noop(NvmeCtrl *n, NvmeRequest *req, bool defval);
uint16_t nvme_get_feature(NvmeCtrl *n, NvmeRequest *req);

uint16_t nvme_set_feature_timestamp(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_set_feature_fdp_events(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_set_feature_write_atomicity(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_set_feature_temp_threshold(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_set_feature_errrec(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_set_feature_vwc(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_set_feature_numqs(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_set_feature_aec(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_set_feature_hbs(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_set_feature_cmd_set_profile(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_set_feature_fdp(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_set_feature(NvmeCtrl *n, NvmeRequest *req);
#endif /* HW_NVME_FEATURES_H */
