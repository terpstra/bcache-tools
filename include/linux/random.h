/*
 * include/linux/random.h
 *
 * Include file for the random number generator.
 */
#ifndef _LINUX_RANDOM_H
#define _LINUX_RANDOM_H

#include <unistd.h>
#include <sys/syscall.h>
#include <linux/bug.h>

#ifndef SYS_getrandom
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

static inline int getrandom(void *buf, size_t buflen, unsigned int flags)
{
	// getrandom was introduced in linux 3.17. Not every libc has it.
#ifdef SYS_getrandom
	return syscall(SYS_getrandom, buf, buflen, flags);
#else
	int out, fd = open("/dev/urandom", O_RDONLY);
	if (fd == -1) return -1;

	out = read(fd, buf, buflen);
	if (close(fd) == -1) return -1;
	return out;
#endif
}

static inline void get_random_bytes(void *buf, int nbytes)
{
	BUG_ON(getrandom(buf, nbytes, 0) != nbytes);
}

static inline int get_random_int(void)
{
	int v;

	get_random_bytes(&v, sizeof(v));
	return v;
}

#endif /* _LINUX_RANDOM_H */
