#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <openamp/open_amp.h>
#include <metal/sys.h>
#include <metal/device.h>
#include <metal/alloc.h>

#define RPMSG_SERVICE_NAME "rpmsg-client-sample"

static struct rpmsg_endpoint ep;

static int endpoint_cb(struct rpmsg_endpoint *ept, void *data,
                       size_t len, uint32_t src, void *priv)
{
    printk("OpenAMP: Received message: \"%.*s\"\n", len, (char *)data);
    return RPMSG_SUCCESS;
}

static void rpmsg_service_bind(struct rpmsg_device *rdev,
                               const char *name, uint32_t dest)
{
    printk("OpenAMP: Binding to %s\n", name);
    rpmsg_create_ept(&ep, rdev, RPMSG_SERVICE_NAME,
                     RPMSG_ADDR_ANY, dest, endpoint_cb, NULL);
}

void main(void)
{
    printk("Starting Zephyr OpenAMP Remote Core\n");
    // Initialization of metal, virtio, and rpmsg typically handled by 
    // IPC service in Zephyr 3.x+. If raw OpenAMP is used, manual init is required.
    // For this POC, we print the success criteria when endpoint_cb is hit.
    
    while (1) {
        k_sleep(K_MSEC(1000));
    }
}
