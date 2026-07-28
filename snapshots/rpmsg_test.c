/*
 * RPMSG direct test for WCNSS firmware
 * Tests if firmware responds to HAL messages on WLAN_CTRL
 *
 * Cross-compile with: aarch64-linux-gnu-gcc -static -o rpmsg_test rpmsg_test.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <errno.h>

#define RPMSG_ADDR_ANY		0xFFFFFFFF

struct rpmsg_endpoint_info {
	char name[32];
	__u32 src;
	__u32 dst;
};

#define RPMSG_CREATE_EPT_IOCTL	_IOW(0xb5, 0x1, struct rpmsg_endpoint_info)

/* wcn36xx HAL message header */
struct __attribute__((packed)) wcn36xx_hal_msg_header {
	__u16 msg_type;
	__u16 msg_version;
	__u32 len;
};

/* HAL_START_REQ message */
struct __attribute__((packed)) wcn36xx_hal_mac_start_req_params {
	__u32 type;
	__u32 len;
};

#define WCN36XX_HAL_START_REQ  0
#define WCN36XX_HAL_MSG_VERSION0 0
#define HAL_MSG_TIMEOUT_MS     10000

static const char *channels[] = {"WLAN_CTRL", "WCNSS_CTRL", NULL};

int test_channel(const char *ctrl_dev, const char *channel) {
	int ctrl_fd, ret;
	struct rpmsg_endpoint_info ept_info;
	struct pollfd pfd;
	__u8 buf[512];
	struct wcn36xx_hal_msg_header *hdr;
	ssize_t n;

	printf("\n=== Testing channel: %s on %s ===\n", channel, ctrl_dev);

	/* Open control device */
	ctrl_fd = open(ctrl_dev, O_RDWR | O_NONBLOCK);
	if (ctrl_fd < 0) {
		perror("open ctrl_dev");
		return -1;
	}

	/* Create endpoint on channel */
	memset(&ept_info, 0, sizeof(ept_info));
	strncpy(ept_info.name, channel, sizeof(ept_info.name) - 1);
	ept_info.src = RPMSG_ADDR_ANY;
	ept_info.dst = RPMSG_ADDR_ANY;

	ret = ioctl(ctrl_fd, RPMSG_CREATE_EPT_IOCTL, &ept_info);
	if (ret < 0) {
		perror("ioctl CREATE_EPT");
		printf("  Could not create endpoint on %s (errno=%d)\n", 
		       channel, errno);
		close(ctrl_fd);
		return -1;
	}
	printf("  Endpoint created on %s\n", channel);

	/* Set blocking again for poll/read */
	int flags = fcntl(ctrl_fd, F_GETFL, 0);
	fcntl(ctrl_fd, F_SETFL, flags & ~O_NONBLOCK);

	/* Build and send HAL_START_REQ message */
	memset(buf, 0, sizeof(buf));
	hdr = (struct wcn36xx_hal_msg_header *)buf;
	hdr->msg_type = WCN36XX_HAL_START_REQ;
	hdr->msg_version = WCN36XX_HAL_MSG_VERSION0;
	hdr->len = sizeof(struct wcn36xx_hal_msg_header) + 
	           sizeof(struct wcn36xx_hal_mac_start_req_params);

	{
		struct wcn36xx_hal_mac_start_req_params *params = 
			(struct wcn36xx_hal_mac_start_req_params *)(buf + sizeof(*hdr));
		params->type = 0; /* DRIVER_TYPE_PRODUCTION */
		params->len = 0;
	}

	n = write(ctrl_fd, buf, hdr->len);
	if (n < 0) {
		perror("write");
		close(ctrl_fd);
		return -1;
	}
	printf("  Sent %zd bytes: msg_type=%d len=%d\n",
	       n, hdr->msg_type, hdr->len);

	/* Wait for response with timeout */
	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = ctrl_fd;
	pfd.events = POLLIN;

	printf("  Waiting up to %dms for response...\n", HAL_MSG_TIMEOUT_MS);
	ret = poll(&pfd, 1, HAL_MSG_TIMEOUT_MS);
	if (ret < 0) {
		perror("poll");
		close(ctrl_fd);
		return -1;
	}

	if (ret == 0) {
		printf("  -> TIMEOUT - No response in %dms\n", HAL_MSG_TIMEOUT_MS);
		/* Try reading anyway to flush anything */
		n = read(ctrl_fd, buf, sizeof(buf));
		if (n > 0)
			printf("  -> But got %zd bytes after poll!\n", n);
		close(ctrl_fd);
		return 0;
	}

	if (pfd.revents & POLLIN) {
		usleep(100000); /* 100ms to let data arrive */
		n = read(ctrl_fd, buf, sizeof(buf));
		if (n < 0) {
			perror("read");
			close(ctrl_fd);
			return -1;
		}
		printf("  <- GOT RESPONSE! %zd bytes\n", n);
		printf("  Raw hex (%zd bytes): ", n);
		for (int i = 0; i < n && i < 64; i++)
			printf("%02x ", buf[i]);
		printf("\n");

		if (n >= (int)sizeof(struct wcn36xx_hal_msg_header)) {
			struct wcn36xx_hal_msg_header *rhdr = 
				(struct wcn36xx_hal_msg_header *)buf;
			printf("  Response msg_type=%d msg_version=%d len=%d\n",
			       rhdr->msg_type, rhdr->msg_version, rhdr->len);
		}
	}

	close(ctrl_fd);
	return n > 0 ? 1 : 0;
}

int main(int argc, char **argv) {
	const char *ctrl_dev = "/dev/rpmsg_ctrl1";
	int i;

	if (argc > 1)
		ctrl_dev = argv[1];

	printf("==================================\n");
	printf("RPMSG WCNSS Firmware Direct Test\n");
	printf("==================================\n");
	printf("Control device: %s\n\n", ctrl_dev);

	/* Try each channel */
	for (i = 0; channels[i]; i++) {
		int result = test_channel(ctrl_dev, channels[i]);
		if (result > 0) {
			printf("\n*** Channel %s WORKS! Firmware responds! ***\n", 
			       channels[i]);
		} else if (result == 0) {
			printf("\nChannel %s: no response (timeout or nothing received)\n",
			       channels[i]);
		} else {
			printf("\nChannel %s: error (see above)\n", channels[i]);
		}
	}

	printf("\nDone.\n");
	return 0;
}
