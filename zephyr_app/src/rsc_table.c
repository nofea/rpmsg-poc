#include <openamp/open_amp.h>
#include <metal/sys.h>

#define SHM_BASE 0xA5000000
#define VRING_TX 0xA5000000
#define VRING_RX 0xA5004000
#define VRING_ALIGN 4096
#define NUM_VRINGS 2

/* Resource table for the AM64x R5F */
struct fw_resource_table {
    unsigned int ver;
    unsigned int num;
    unsigned int reserved[2];
    unsigned int offset[1];
    struct fw_rsc_vdev vdev;
    struct fw_rsc_vdev_vring vring0;
    struct fw_rsc_vdev_vring vring1;
} __attribute__((packed));

const struct fw_resource_table resource_table __attribute__((section(".resource_table"))) = {
    1, 1, {0, 0},
    { offsetof(struct fw_resource_table, vdev) },
    {
        RSC_VDEV, 7, 0, 0, 0, NUM_VRINGS, {0, 0},
    },
    { VRING_TX, VRING_ALIGN, 256, 1, 0 },
    { VRING_RX, VRING_ALIGN, 256, 2, 0 },
};
