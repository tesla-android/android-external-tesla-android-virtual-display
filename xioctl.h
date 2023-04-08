#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>

#include <sys/ioctl.h>


#ifndef US_CFG_XIOCTL_RETRIES
#	define US_CFG_XIOCTL_RETRIES 4
#endif
#define _XIOCTL_RETRIES ((unsigned)(US_CFG_XIOCTL_RETRIES))


INLINE int us_xioctl(int fd, int request, void *arg) {
	int retries = _XIOCTL_RETRIES;
	int retval = -1;

	do {
		retval = ioctl(fd, request, arg);
	} while (
		retval
		&& retries--
		&& (
			errno == EINTR
			|| errno == EAGAIN
			|| errno == ETIMEDOUT
		)
	);
	return retval;
}

#ifdef __cplusplus
}
#endif
