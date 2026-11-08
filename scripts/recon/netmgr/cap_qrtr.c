/* cap_qrtr — LD_PRELOAD shim: hex-dump every send/recv on AF_QIPCRTR sockets
 * to /root/wda-raw.log. Read-only instrumentation; no behavior change.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <time.h>

#define AF_QIPCRTR 42

static int is_qrtr(const struct sockaddr *sa, socklen_t l)
{
	return sa && l >= sizeof(sa_family_t) && sa->sa_family == AF_QIPCRTR;
}

static void dump(const char *dir, const void *buf, size_t len)
{
	FILE *f = fopen("/root/wda-raw.log", "a");
	struct timespec ts;
	size_t i;

	if (!f)
		return;
	clock_gettime(CLOCK_REALTIME, &ts);
	fprintf(f, "[%lld.%03ld] %s %zu bytes:",
		(long long)ts.tv_sec, ts.tv_nsec / 1000000, dir, len);
	for (i = 0; i < len; i++)
		fprintf(f, "%s%02x", (i % 16) ? " " : "\n  ", ((const unsigned char *)buf)[i]);
	fprintf(f, "\n");
	fclose(f);
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags,
	       const struct sockaddr *dest, socklen_t dlen)
{
	static ssize_t (*real)(int, const void *, size_t, int,
			       const struct sockaddr *, socklen_t);
	if (!real)
		real = dlsym(RTLD_NEXT, "sendto");
	if (is_qrtr(dest, dlen))
		dump("TX sendto", buf, len);
	return real(fd, buf, len, flags, dest, dlen);
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
	static ssize_t (*real)(int, const void *, size_t, int);
	struct sockaddr_storage ss;
	socklen_t l = sizeof(ss);
	if (!real)
		real = dlsym(RTLD_NEXT, "send");
	if (getsockname(fd, (struct sockaddr *)&ss, &l) == 0 &&
	    is_qrtr((struct sockaddr *)&ss, l))
		dump("TX send", buf, len);
	return real(fd, buf, len, flags);
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
	static ssize_t (*real)(int, const struct msghdr *, int);
	struct sockaddr_storage ss;
	socklen_t l = sizeof(ss);
	if (!real)
		real = dlsym(RTLD_NEXT, "sendmsg");
	if (getsockname(fd, (struct sockaddr *)&ss, &l) == 0 &&
	    is_qrtr((struct sockaddr *)&ss, l) && msg && msg->msg_iov)
		dump("TX sendmsg", msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len);
	return real(fd, msg, flags);
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
		 struct sockaddr *src, socklen_t *slen)
{
	static ssize_t (*real)(int, void *, size_t, int, struct sockaddr *,
			       socklen_t *);
	ssize_t rc;
	if (!real)
		real = dlsym(RTLD_NEXT, "recvfrom");
	rc = real(fd, buf, len, flags, src, slen);
	if (rc > 0 && src && is_qrtr(src, slen ? *slen : 0))
		dump("RX recvfrom", buf, rc);
	return rc;
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
	static ssize_t (*real)(int, void *, size_t, int);
	struct sockaddr_storage ss;
	socklen_t l = sizeof(ss);
	ssize_t rc;
	if (!real)
		real = dlsym(RTLD_NEXT, "recv");
	rc = real(fd, buf, len, flags);
	if (rc > 0 && getsockname(fd, (struct sockaddr *)&ss, &l) == 0 &&
	    is_qrtr((struct sockaddr *)&ss, l))
		dump("RX recv", buf, rc);
	return rc;
}

ssize_t recvmsg(int fd, struct msghdr *msg, int flags)
{
	static ssize_t (*real)(int, struct msghdr *, int);
	struct sockaddr_storage ss;
	socklen_t l = sizeof(ss);
	ssize_t rc;
	if (!real)
		real = dlsym(RTLD_NEXT, "recvmsg");
	rc = real(fd, msg, flags);
	if (rc > 0 && getsockname(fd, (struct sockaddr *)&ss, &l) == 0 &&
	    is_qrtr((struct sockaddr *)&ss, l) && msg && msg->msg_iov)
		dump("RX recvmsg", msg->msg_iov[0].iov_base, rc);
	return rc;
}
