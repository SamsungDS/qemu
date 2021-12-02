/*
 * QEMU NVMe-MI Controller
 *
 * Copyright (c) 2021, Samsung Electronics co Ltd.
 *
 * Written by Padmakar Kalghatgi <p.kalghatgi@samsung.com>
 *
 * This code is licensed under the GNU GPL v2 or later.
 *
 * This module acts as a host slave, to which the QEMU-MI module
 * will post the response to.
 *
 * Need to use as following to enable this device
 * -device nvme-mi-i2c-slave, addr=<slaveaddr>
 */

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-core.h"
#include "hw/block/block.h"
#include "nvme-mi-slave.h"

static uint8_t nvme_mi_slave_i2c_recv(I2CSlave *s)
{
    Nvmemislave *mislave = (Nvmemislave *)s;

    if (mislave->syncflag == true) {
        return -1;
    }
    return mislave->recvbuffer[mislave->recvlen++];
}

static int nvme_mi_slave_i2c_send(I2CSlave *s, uint8_t data)
{
    Nvmemislave *mislave = (Nvmemislave *)s;
    mislave->syncflag = true;

    switch (mislave->pktpos) {
    case NVME_MI_BYTE_LENGTH_POS:
        mislave->pktlen = data + 1;
        break;
    case NVME_MI_EOM_POS:
        mislave->eom = (data >> 6) & 1;
        break;
    }
    mislave->recvbuffer[mislave->sendlen++] = data;
    mislave->pktpos++;
    if (mislave->pktpos == mislave->pktlen + 3) {
        mislave->pktlen = 0;
        mislave->pktpos = 0;

        if (mislave->eom == 1) {
            mislave->sendlen = 0;
            mislave->recvlen = 0;
            mislave->eom = 0;
            mislave->syncflag = false;
        }
    }
    return 0;
}

static void nvme_mi_slave_realize(DeviceState *dev, Error **errp)
{
    Nvmemislave *mislave = (Nvmemislave *)dev;
    mislave->sendlen = 0;
    mislave->recvlen = 0;
    mislave->eom = 0;
    mislave->syncflag = false;
}

static void nvme_mi_slave_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = nvme_mi_slave_realize;
    k->recv = nvme_mi_slave_i2c_recv;
    k->send = nvme_mi_slave_i2c_send;
}

static const TypeInfo nvme_mi_slave = {
    .name = TYPE_NVME_MI_SLAVE,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Nvmemislave),
    .class_init = nvme_mi_slave_class_init,
};

static void register_types(void)
{
    type_register_static(&nvme_mi_slave);
}

type_init(register_types);
