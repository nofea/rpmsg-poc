#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <openamp/open_amp.h>
#include <metal/sys.h>
#include <metal/io.h>
#include "rsc_table.h"

#define RPMSG_SERVICE_NAME "rpmsg-client-sample"
#define PING_MSG           "ping"
#define REPLY_MSG          "pong"

/* 512-byte RPMsg buffer minus the 16-byte RPMsg header */
#define MAX_PAYLOAD 496

#define SHM_NODE DT_CHOSEN(zephyr_ipc_shm)
#define SHM_ADDR DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE DT_REG_SIZE(SHM_NODE)

/* ZynqMP IPI: RPU0 (channel 1) <-> APU (channel 7), see app.overlay */
static const struct mbox_dt_spec mbox_tx = MBOX_DT_SPEC_GET(DT_NODELABEL(rpmsg_mbox), tx);
static const struct mbox_dt_spec mbox_rx = MBOX_DT_SPEC_GET(DT_NODELABEL(rpmsg_mbox), rx);

static K_SEM_DEFINE(kick_sem, 0, 1);

static metal_phys_addr_t shm_physmap = SHM_ADDR;
static struct metal_io_region shm_io;
static metal_phys_addr_t rsc_physmap;
static struct metal_io_region rsc_io;

static struct rpmsg_virtio_device rvdev;
static struct rpmsg_endpoint ept;

/* Set once a message from the A53 has told us its endpoint address */
static atomic_t a53_connected;

static void mbox_rx_callback(const struct device *dev, mbox_channel_id_t channel_id,
			     void *user_data, struct mbox_msg *data)
{
	k_sem_give(&kick_sem);
}

/* Called by OpenAMP whenever the A53 has to look at a vring */
static int mailbox_notify(void *priv, uint32_t id)
{
	int ret;

	/* The IPI is busy until the A53 acknowledges the previous kick */
	while ((ret = mbox_send_dt(&mbox_tx, NULL)) == -EBUSY) {
		k_msleep(1);
	}

	return ret;
}

static int endpoint_cb(struct rpmsg_endpoint *ept, void *data, size_t len, uint32_t src,
		       void *priv)
{
	/* OpenAMP has now filled ept->dest_addr with the A53's address */
	if (!atomic_set(&a53_connected, 1)) {
		printk("OpenAMP: A53 connected (addr 0x%x)\n", src);
	}

	/* A zero-length message only announces the A53's endpoint */
	if (len == 0) {
		return RPMSG_SUCCESS;
	}

	printk("OpenAMP: Received message: \"%.*s\"\n", (int)len, (char *)data);

	if (len == strlen(PING_MSG) && !memcmp(data, PING_MSG, len)) {
		if (rpmsg_send(ept, REPLY_MSG, strlen(REPLY_MSG)) < 0) {
			printk("OpenAMP: failed to send reply\n");
		} else {
			printk("OpenAMP: Sent reply: \"%s\"\n", REPLY_MSG);
		}
	}

	return RPMSG_SUCCESS;
}

/*
 * rpmsg send <text...>: runs in the shell thread. OpenAMP serializes vring
 * access with the RPMsg device lock, so this can race with main()'s kick
 * processing.
 */
static int cmd_rpmsg_send(const struct shell *sh, size_t argc, char **argv)
{
	char msg[MAX_PAYLOAD];
	size_t len = 0;
	int ret;

	if (!atomic_get(&a53_connected)) {
		shell_error(sh, "OpenAMP: A53 not connected, start am64_rpmsg_userspace first");
		return -ENOTCONN;
	}

	for (size_t i = 1; i < argc; i++) {
		size_t n = strlen(argv[i]);

		if (len + (i > 1) + n > MAX_PAYLOAD) {
			shell_error(sh, "OpenAMP: message too long (max %d bytes)", MAX_PAYLOAD);
			return -EMSGSIZE;
		}
		if (i > 1) {
			msg[len++] = ' ';
		}
		memcpy(&msg[len], argv[i], n);
		len += n;
	}

	ret = rpmsg_send(&ept, msg, len);
	if (ret < 0) {
		shell_error(sh, "OpenAMP: failed to send message: %d", ret);
		return -EIO;
	}
	shell_print(sh, "OpenAMP: Sent message: \"%.*s\"", (int)len, msg);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_rpmsg,
	SHELL_CMD_ARG(send, NULL, "Send a text message to the A53: rpmsg send <text...>",
		      cmd_rpmsg_send, 2, SHELL_OPT_ARG_CHECK_SKIP),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(rpmsg, &sub_rpmsg, "RPMsg commands", NULL);

static struct virtio_device *create_vdev(struct am64_rsc_table *rsc)
{
	struct virtio_device *vdev;
	uint8_t status = 0;

	vdev = rproc_virtio_create_vdev(VIRTIO_DEV_DEVICE, VDEV_NOTIFYID, &rsc->vdev, &rsc_io,
					NULL, mailbox_notify, NULL);
	if (!vdev) {
		return NULL;
	}

	/* The A53 sets DRIVER_OK once it has initialized the vrings and buffers */
	printk("OpenAMP: waiting for the A53 to initialize the virtio device\n");
	while (!(status & VIRTIO_CONFIG_STATUS_DRIVER_OK)) {
		k_msleep(10);
		virtio_get_status(vdev, &status);
	}

	if (rproc_virtio_init_vring(vdev, 0, rsc->vring0.notifyid,
				    (void *)(uintptr_t)rsc->vring0.da, &shm_io,
				    rsc->vring0.num, rsc->vring0.align) ||
	    rproc_virtio_init_vring(vdev, 1, rsc->vring1.notifyid,
				    (void *)(uintptr_t)rsc->vring1.da, &shm_io,
				    rsc->vring1.num, rsc->vring1.align)) {
		rproc_virtio_remove_vdev(vdev);
		return NULL;
	}

	return vdev;
}

int main(void)
{
	struct metal_init_params metal_params = METAL_INIT_DEFAULTS;
	struct am64_rsc_table *rsc;
	struct virtio_device *vdev;
	struct rpmsg_device *rdev;
	int ret;

	printk("Starting Zephyr OpenAMP remote on R5F\n");

	ret = metal_init(&metal_params);
	if (ret) {
		printk("OpenAMP: metal_init failed: %d\n", ret);
		return ret;
	}

	rsc = rsc_table_publish();
	rsc_physmap = (uintptr_t)rsc;
	metal_io_init(&rsc_io, rsc, &rsc_physmap, sizeof(*rsc), -1, 0, NULL);
	metal_io_init(&shm_io, (void *)SHM_ADDR, &shm_physmap, SHM_SIZE, -1, 0, NULL);
	printk("OpenAMP: resource table published at %p\n", rsc);

	ret = mbox_register_callback_dt(&mbox_rx, mbox_rx_callback, NULL);
	if (!ret) {
		ret = mbox_set_enabled_dt(&mbox_rx, true);
	}
	if (ret) {
		printk("OpenAMP: IPI mailbox setup failed: %d\n", ret);
		return ret;
	}

	vdev = create_vdev(rsc);
	if (!vdev) {
		printk("OpenAMP: failed to create virtio device\n");
		return -ENODEV;
	}

	ret = rpmsg_init_vdev(&rvdev, vdev, NULL, &shm_io, NULL);
	if (ret) {
		printk("OpenAMP: rpmsg_init_vdev failed: %d\n", ret);
		return ret;
	}
	rdev = rpmsg_virtio_get_rpmsg_device(&rvdev);

	/* Creating the endpoint sends the name service announcement to the A53 */
	ret = rpmsg_create_ept(&ept, rdev, RPMSG_SERVICE_NAME, RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
			       endpoint_cb, NULL);
	if (ret) {
		printk("OpenAMP: rpmsg_create_ept failed: %d\n", ret);
		return ret;
	}
	printk("OpenAMP: endpoint \"%s\" announced, waiting for messages\n", RPMSG_SERVICE_NAME);

	while (1) {
		k_sem_take(&kick_sem, K_FOREVER);
		rproc_virtio_notified(vdev, RSC_NOTIFY_ID_ANY);
	}

	return 0;
}
