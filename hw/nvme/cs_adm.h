#ifndef HW_NVME_CS_ADM_H
#define HW_NVME_CS_ADM_H

typedef struct NvmeCtrl NvmeCtrl;
typedef struct NvmeRequest NvmeRequest;
typedef struct NvmeCmdSet NvmeCmdSet;

uint16_t nvme_del_sq(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_create_sq(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_del_cq(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_create_cq(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_abort(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_aer(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_ns_attachment(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_directive_send(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_virt_mngmt(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_directive_receive(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_dbbuf_config(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_format(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_security_send(NvmeCtrl *n, NvmeRequest *req);
uint16_t nvme_security_receive(NvmeCtrl *n, NvmeRequest *req);

void nvme_cse_acs_ioctrl23_m(NvmeCmdSet *tbl);
void nvme_cse_acs_defaults(NvmeCmdSet *tbl);
#endif /* HW_NVME_CS_ADM */
