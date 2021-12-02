#ifndef NVMEMISLAVEH
#define NVMEMISLAVEH

#include "hw/i2c/i2c.h"
#define TYPE_NVME_MI_SLAVE "nvme-mi-i2c-slave"

#define MAX_NVME_MI_BUF_SIZE 5000

enum Nvmemislavepktpos
{
   NVME_MI_ADDR_POS = 0,
   NVME_MI_BYTE_LENGTH_POS = 2,
   NVME_MI_EOM_POS = 7
};

typedef struct Nvmemislave
{
    I2CSlave parent_obj;
    uint32_t sendlen;
    uint32_t recvlen;
    uint32_t pktpos;
    uint32_t pktlen;
    uint8_t eom;
    bool syncflag;
    u_char recvbuffer[MAX_NVME_MI_BUF_SIZE];
} Nvmemislave;

#endif