/*
 * QEMU NVMe-MI
 *
 * Copyright (c) 2021 Samsung Electronics Co., Ltd.
 *
 * Authors:
 *   Padmakar Kalghatgi      <p.kalghatgi@samsung.com>
 *   Arun Kumar Agasar       <arun.kka@samsung.com>
 *   Saurav Kumar            <saurav.29@partner.samsung.com>
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

#ifndef NVME_MI_H
#define NVME_MI_H

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include "hw/i2c/i2c.h"

#define TYPE_NVME_MI "nvme-mi-i2c"

#define NVM_SUBSYSTEM_INFORMATION 0
#define PORT_INFORMATION 1
#define CONTROLLER_LIST 2
#define CONTROLLER_INFORMATION 3
#define OPT_SUPP_CMD_LIST 4
#define MGMT_EPT_BUFF_CMD_SUPP_LIST 5

/*
 *  considering MCTP transmission unit size
 *  being 64, total MI payload size equal to 4224
 *  and further including the SMBUS header for
 *  each of the MCTP packet, I have defined maximum
 *  buffer size of 5000
 */
#define MAX_NVME_MI_BUF_SIZE 5000
#define NVME_MI_SMBUS_HEADER_AND_PEC 9

/* value of 1 for the frequency means 100Khz */
#define NVME_MI_DEF_SMBUS_FREQ 1
#define NVME_MI_DEF_MCTP_TRANS_UNIT_SIZE 64

enum NvmeMiMngmtInterfaceCmdSetsOpcodes {
   READ_NVME_MI_DS                   = 0x00,
   NVM_SHSP                          = 0x01,
   CHSP                              = 0x02,
   CONFIGURATION_SET                 = 0x03,
   CONFIGURATION_GET                 = 0x04,
   VPD_READ                          = 0x05,
   VPD_WRITE                         = 0x06,
   MI_RESET                          = 0x07,
   SES_RECEIVE                       = 0x08,
   SES_SEND                          = 0x09,
   MANAGEMENT_ENDPOINT_BUFFER_READ   = 0x0A,
   MANAGEMENT_ENDPOINT_BUFFER_WRITE  = 0x0B,
   MI_RESERVED                       = 0x0C,
   VENDOR_SPECIFIC                   = 0xC0
};

enum NvmeMiControlPrimitiveOpcodes {
   PAUSE                             = 0x00,
   RESUME                            = 0x01,
   ABORT                             = 0x02,
   GET_STATE                         = 0x03,
   REPLAY                            = 0x04,
   CTRL_PRIMITIVE_RESERVED           = 0x05,
   CTRL_PRIMITIVE_VENDOR_SPECIFIC    = 0xF0
};

enum NvmeMiType {
    CP,
    NVME_MI_CMD,
    NVME_ADM_CMD
};

enum NvmeMiConfigGetResponseValue {
   DEFAULT_MCTP_SIZE   = 64,
   DEFAULT_SMBUS_FREQ  = 1,
   SET_SMBUS_FREQ      = 129,
   SET_7BITS           = 255,
   SET_4BITS           = 15,
   SET_16BITS          = 65535
};

enum NvmeMiConfigurationIdentifier {
   SMBUS_I2C_FREQ = 1,
   HEALTH_STATUS_CHG,
   MCTP_TRANS_UNIT_SIZE,
};

enum NvmeMiResponseMessageStatus {
   SUCCESS,
   MORE_PROCESSING_REQUIRED,
   INTERNAL_ERROR,
   INVALID_COMMAND_OPCODE,
   INVALID_PARAMETER,
   INVALID_COMMAND_SIZE,
   INVALID_COMMAND_INPUT_DATA_SIZE,
   ACCESS_DENIED,
   VPD_UPDATES_EXCEEDED = 0x20,
   PCIe_INACCESSIBLE
};

uint32_t NvmeMiCmdOptSupList[] = {
  /*
   * MANAGEMENT_ENDPOINT_BUFFER_READ,
   * MANAGEMENT_ENDPOINT_BUFFER_WRITE,
   */
};

uint32_t NvmeMiAdminCmdOptSupList[] = {
   /*
    *  NVME_ADM_CMD_DST,
    *  NVME_ADM_CMD_DOWNLOAD_FW,
    *  NVME_ADM_CMD_ACTIVATE_FW,
    *  NVME_ADM_CMD_FORMAT_NVM,
    *  NVME_ADM_CMD_NS_MANAGEMENT,
    *  NVME_ADM_CMD_NS_ATTACHMENT,
    *  NVME_ADM_CMD_DIRECTIVE_SEND,
    *  NVME_ADM_CMD_DIRECTIVE_RECV,
    *  NVME_ADM_CMD_SET_FEATURES,
    *  NVME_ADM_CMD_SANITIZE,
    */
};

enum NvmemiPktPos {
   NVME_MI_BYTE_LENGTH_POS = 1,
   NVME_MI_HOST_SLAVE_ADDR_POS = 2,
   NVME_MI_EOM_POS = 6
};

typedef struct pktposstate {
  u_char sendrecvbuf[MAX_NVME_MI_BUF_SIZE];
  uint32_t pktlen, pktpos, mode;
} pktposstate;

typedef struct NvmeMiSendRecvStruct {
   uint32_t total_len;
   uint32_t offset;
   uint8_t eom;
   pktposstate state;
   uint8_t *cmdbuffer;
   uint8_t hostslaveaddr;
} NvmeMiSendRecvStruct;

typedef struct NvmeMiVpdElements {
    long common_header;
} NvmeMiVpdElements;

typedef struct NvmeMiCtrl {
   I2CSlave parent_obj;
   uint32_t mctp_unit_size;
   uint32_t smbus_freq;
   NvmeMiVpdElements vpd_data;
   NvmeMiSendRecvStruct  misendrecv;
   NvmeCtrl *n;
   I2CBus *bus;
} NvmeMiCtrl;

typedef struct NvmeMiMessageHeader {
   uint32_t msgtype:7;
   uint32_t ic:1;
   uint32_t csi:1;
   uint32_t reserved:2;
   uint32_t nmimt:4;
   uint32_t ror:1;
   uint32_t reserved1:16;
} NvmeMiMessageHeader;

typedef struct NvmeMiRequest {
   NvmeMiMessageHeader msg_header;
   uint32_t               opc:8;
   uint32_t               rsvd:24;
   uint32_t               dword0;
   uint32_t               dword1;
   uint32_t               mic;
} NvmeMiRequest;

typedef struct NvmeAdminMiRequest {
   NvmeMiMessageHeader msg_header;
   uint8_t                opc;
   uint8_t                cmdflags;
   uint16_t               cntlid;
   uint32_t               sqentry1;
   uint32_t               sqentry2;
   uint32_t               sqentry3;
   uint32_t               sqentry4;
   uint32_t               sqentry5;
   uint32_t               dataofst;
   uint32_t               datalen;
   uint32_t               reserved[2];
   uint32_t               sqentry10;
   uint32_t               sqentry11;
   uint32_t               sqentry12;
   uint32_t               sqentry13;
   uint32_t               sqentry14;
   uint32_t               sqentry15;
   uint32_t               mic;
} NvmeAdminMiRequest;

typedef struct ReadNvmeMiDs {
    uint16_t cntrlid;
    uint8_t  portlid;
    uint8_t  dtyp;
}  ReadNvmeMiDs;

typedef struct NvmeMiConfigurationSet {
    uint8_t conf_identifier_dword_0;
    uint16_t conf_identifier_specific_dword_0;
    uint16_t conf_identifier_specific_dword_1;
}  MiConfigurationSet;

typedef struct NvmeMiNvmSubsysHspds {
    uint8_t nss;
    uint8_t sw;
    uint8_t ctemp;
    uint8_t pdlu;
    uint16_t ccs;
    uint16_t reserved;
} NvmeMiNvmSubsysHspds;

typedef struct NvmeMiControlPrimitives {
    uint32_t nmh;
    uint32_t cpo;
    uint32_t tag;
    uint32_t cpsp;
    uint32_t mic;
} NvmeMiControlPrimitives;

typedef struct NvmMiSubsysInfoDs {
    uint8_t nump;
    uint8_t mjr;
    uint8_t mnr;
    uint8_t rsvd[29];
} NvmMiSubsysInfoDs;

typedef struct NvmeMiCwarnStruct {
    uint8_t spare_thresh:1;
    uint8_t temp_above_or_under_thresh:1;
    uint8_t rel_degraded:1;
    uint8_t read_only:1;
    uint8_t vol_mem_bup_fail:1;
    uint8_t reserved:3;
} NvmeMiCwarnStruct;

typedef struct NvmeMiCstsStruct {
    uint16_t rdy:1;
    uint16_t cfs:1;
    uint16_t shst:2;
    uint16_t nssro:1;
    uint16_t en:1;
    uint16_t nssac:1;
    uint16_t fwact:1;
    uint16_t reserved:8;
} NvmeMiCstsStruct;

typedef struct NvmeMiCtrlHealthDs {
   uint16_t ctlid;
   NvmeMiCstsStruct csts;
   uint16_t ctemp;
   uint16_t pdlu;
   uint8_t spare;
   NvmeMiCwarnStruct cwarn;
   uint8_t reserved[7];
} NvmeMiCtrlHealthDs;

typedef struct NvmeMiResponse {
   NvmeMiMessageHeader msg_header;
   uint32_t status:8;
   uint32_t mgmt_resp:24;
} NvmeMiResponse;

typedef struct NvmeMiAdminResponse {
   NvmeMiMessageHeader msg_header;
   uint32_t status:8;
   uint32_t mgmt_resp:24;
   uint32_t cqdword0;
   uint32_t cqdword1;
   uint32_t cqdword3;
} NvmeMiAdminResponse;



#endif
