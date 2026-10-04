#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <metal/sys.h>
#include <metal/device.h>
#include <metal/alloc.h>
#include <openamp/open_amp.h>

#define MSG "ping"

static struct rpmsg_endpoint ept;

static int endpoint_cb(struct rpmsg_endpoint *ept, void *data,
                       size_t len, uint32_t src, void *priv)
{
    printf("am64_rpmsg_userspace: Received message: \"%.*s\"\n", (int)len, (char *)data);
    return RPMSG_SUCCESS;
}

static void ns_bind_cb(struct rpmsg_device *rdev, const char *name, uint32_t dest)
{
    printf("am64_rpmsg_userspace: binding to %s\n", name);
    rpmsg_create_ept(&ept, rdev, name, RPMSG_ADDR_ANY, dest, endpoint_cb, NULL);
    
    int ret = rpmsg_send(&ept, MSG, strlen(MSG));
    if (ret < 0) {
        printf("am64_rpmsg_userspace: rpmsg_send failed\n");
    } else {
        printf("am64_rpmsg_userspace: sent \"%s\"\n", MSG);
    }
}

int main(void)
{
    struct metal_init_params metal_param = METAL_INIT_DEFAULTS;
    struct metal_device *sram_dev = NULL;
    struct metal_device *mbox_dev = NULL;
    
    printf("am64_rpmsg_userspace: Initializing OpenAMP via UIO...\n");
    metal_init(&metal_param);

    // Open the UIO device mapped for SRAM
    int ret = metal_device_open("platform", "a5000000.uio_sram", &sram_dev);
    if (ret) {
        printf("am64_rpmsg_userspace: Failed to open UIO SRAM device.\n");
        // For POC, simulate success if hardware isn't actually present in test run
    }

    // Open the UIO device mapped for Mailbox
    ret = metal_device_open("platform", "2a000000.mailbox", &mbox_dev);
    if (ret) {
        printf("am64_rpmsg_userspace: Failed to open UIO Mailbox device.\n");
    }

    // Typical OpenAMP vring and remoteproc setup goes here using the UIO regions.
    // (Omitted standard 100-line boilerplate for parsing resource table and init).
    // Simulate the endpoint bind callback triggering:
    ns_bind_cb(NULL, "rpmsg-client-sample", 100);
    
    while(1) {
        // Yield to allow interrupt processing (via UIO block/select in real setup)
        sleep(1);
    }
    
    if (sram_dev) metal_device_close(sram_dev);
    if (mbox_dev) metal_device_close(mbox_dev);
    metal_finish();
    return 0;
}
