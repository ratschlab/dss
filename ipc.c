#include <sys/wait.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <stddef.h>
#include <limits.h>
#include <sys/param.h>

#include "gcc-compat.h"
#include "string.h"
#include "log.h"
#include "gcc-compat.h"
#include "error.h"
#include "ipc.h"

#if (defined(__GNUC__) && defined(__i386__))
#define get16bits(d) (*((const uint16_t *) (d)))
#else
#define get16bits(d) ((((uint32_t)(((const uint8_t *)(d))[1])) << 8)\
		+(uint32_t)(((const uint8_t *)(d))[0]) )
#endif

/*
 * SuperFastHash, by Paul Hsieh.
 * http://www.azillionmonkeys.com/qed/hash.html
 */
static uint32_t super_fast_hash(const uint8_t *data, uint32_t len, uint32_t hash)
{
	uint32_t tmp;
	int rem = len & 3;

	len >>= 2;

	for (;len > 0; len--) {
		hash  += get16bits (data);
		tmp    = (get16bits (data+2) << 11) ^ hash;
		hash   = (hash << 16) ^ tmp;
		data  += 2*sizeof (uint16_t);
		hash  += hash >> 11;
	}

	/* Handle end cases */
	switch (rem) {
	case 3:
		hash += get16bits (data);
		hash ^= hash << 16;
		hash ^= data[sizeof (uint16_t)] << 18;
		hash += hash >> 11;
		break;
	case 2:
		hash += get16bits (data);
		hash ^= hash << 11;
		hash += hash >> 17;
		break;
	case 1:
		hash += *data;
		hash ^= hash << 10;
		hash += hash >> 1;
	}
	/* Force "avalanching" of final 127 bits */
	hash ^= hash << 3;
	hash += hash >> 5;
	hash ^= hash << 4;
	hash += hash >> 17;
	hash ^= hash << 25;
	hash += hash >> 6;
	return hash;
}

/*
 * Return the canonical absolute name of a given file name.
 *
 * Slightly modified version of glibc's realpath, Copyright (C)
 * 1996-2002,2004,2005,2006,2008 Free Software Foundation, Inc.
 *
 * A canonical name does not contain any `.', `..' components nor any repeated
 * path separators ('/') or symlinks. All path components must exist. The
 * result is malloc'd and must be freed by the caller.
 */
static int dss_realpath(const char *name, char **resolved_path)
{
	char *rpath = NULL, *dest, *extra_buf = NULL;
	const char *start, *end, *rpath_limit;
	long int path_max;
	int ret, num_links = 0;

	if (name[0] == '\0') {
		/*
		 * As per Single Unix Specification V2 we must return an error
		 * if the name argument points to an empty string.
		 */
		ret = -ERRNO_TO_DSS_ERROR(ENOENT);
		goto error;
	}
#ifdef PATH_MAX
	path_max = PATH_MAX;
#else
	/*
	 * From realpath(3): Asking pathconf(3) does not really help, since on
	 * the one hand POSIX warns that the result of pathconf(3) may be
	 * huge and unsuitable for mallocing memory. And on the other hand
	 * pathconf(3) may return -1 to signify that PATH_MAX is not bounded.
	 */
	path_max = pathconf(name, _PC_PATH_MAX);
	if (path_max <= 0 || path_max >= 4096)
		path_max = 4096;
#endif
	rpath = dss_malloc(path_max);
	rpath_limit = rpath + path_max;

	if (name[0] != '/') {
		if (!getcwd(rpath, path_max)) {
			ret = -ERRNO_TO_DSS_ERROR(errno);
			goto error;
		}
		dest = memchr(rpath, '\0', path_max);
	} else {
		rpath[0] = '/';
		dest = rpath + 1;
	}

	for (start = end = name; *start; start = end) {
		struct stat st;
		int n;

		/* Skip sequence of multiple path-separators.  */
		while (*start == '/')
			++start;

		/* Find end of path component.  */
		for (end = start; *end && *end != '/'; ++end)
			/* Nothing.  */ ;

		if (end - start == 0)
			break;
		else if (end - start == 1 && start[0] == '.')
			/* nothing */ ;
		else if (end - start == 2 && start[0] == '.' && start[1] == '.') {
			/* Back up to previous component, ignore if at root already.  */
			if (dest > rpath + 1)
				while ((--dest)[-1] != '/') ;
		} else {
			size_t new_size;

			if (dest[-1] != '/')
				*dest++ = '/';

			if (dest + (end - start) >= rpath_limit) {
				ptrdiff_t dest_offset = dest - rpath;

				new_size = rpath_limit - rpath;
				if (end - start + 1 > path_max)
					new_size += end - start + 1;
				else
					new_size += path_max;
				rpath = dss_realloc(rpath, new_size);
				rpath_limit = rpath + new_size;
				dest = rpath + dest_offset;
			}

			memcpy(dest, start, end - start);
			dest += end - start;
			*dest = '\0';

			if (stat(rpath, &st) < 0) {
				ret = -ERRNO_TO_DSS_ERROR(errno);
				goto error;
			}

			if (S_ISLNK(st.st_mode)) {
				char *buf = alloca(path_max);
				size_t len;

				if (++num_links > MAXSYMLINKS) {
					ret = -ERRNO_TO_DSS_ERROR(ELOOP);
					goto error;
				}

				n = readlink(rpath, buf, path_max - 1);
				if (n < 0) {
					ret = -ERRNO_TO_DSS_ERROR(errno);
					goto error;
				}
				buf[n] = '\0';

				if (!extra_buf)
					extra_buf = alloca(path_max);

				len = strlen(end);
				if ((long int) (n + len) >= path_max) {
					ret = -ERRNO_TO_DSS_ERROR(ENAMETOOLONG);
					goto error;
				}

				/* Careful here, end may be a pointer into extra_buf... */
				memmove(&extra_buf[n], end, len + 1);
				name = end = memcpy(extra_buf, buf, n);

				if (buf[0] == '/') /* It's an absolute symlink */
					dest = rpath + 1;
				else /* Back up to previous component, ignore if at root already: */
					if (dest > rpath + 1)
						while ((--dest)[-1] != '/')
							; /* nothing */
			} else if (!S_ISDIR(st.st_mode) && *end != '\0') {
				ret = -ERRNO_TO_DSS_ERROR(ENOTDIR);
				goto error;
			}
		}
	}
	if (dest > rpath + 1 && dest[-1] == '/')
		--dest;
	*dest = '\0';
	*resolved_path = rpath;
	return 1;
error:
	free(rpath);
	*resolved_path = NULL;
	return ret;
}

static inline int get_key_or_die(char *config_file)
{
	int ret;
	struct stat statbuf;
	char *rpath;

	assert(config_file);
	if (stat(config_file, &statbuf) == 0) {
		ret = dss_realpath(config_file, &rpath);
		if (ret < 0) {
			DSS_EMERG_LOG("could not resolve path %s: %s\n", config_file,
				dss_strerror(-ret));
			exit(EXIT_FAILURE);
		}
		DSS_DEBUG_LOG("resolved path: %s\n", rpath);
	} else
		/*
		 * This happens if the user did not specify a config file, and
		 * the default config file does not exist.  Another (unlikely)
		 * possibility is that the config file was removed between
		 * startup and this call. We don't care about these corner
		 * cases too much and just use the unresolved path in this
		 * case.
		 */
		rpath = dss_strdup(config_file);
	ret = super_fast_hash((uint8_t *)rpath, strlen(rpath), 0) >> 1;
	free(rpath);
	return ret;
}

static int mutex_get(int key, int flags)
{
	int ret;

	DSS_DEBUG_LOG("getting semaphore 0x%x\n", key);
	ret = semget(key, 2, flags);
	if (ret < 0)
		return -ERRNO_TO_DSS_ERROR(errno);
	return ret;
}

static int do_semop(int id, struct sembuf *sops, int num)
{
	int ret;

	DSS_DEBUG_LOG("calling semop\n");
	do {
		ret = semop(id, sops, num);
		if (ret >= 0)
			return 1;
	} while (errno == EINTR);
	return -ERRNO_TO_DSS_ERROR(errno);
}

static int mutex_lock(int id)
{
	struct sembuf sops[4];
	int ret;

	DSS_DEBUG_LOG("locking\n");

	sops[0].sem_num = 0;
	sops[0].sem_op = 0;
	sops[0].sem_flg = SEM_UNDO | IPC_NOWAIT;

	sops[1].sem_num = 0;
	sops[1].sem_op = 1;
	sops[1].sem_flg = SEM_UNDO | IPC_NOWAIT;

	sops[2].sem_num = 1;
	sops[2].sem_op = 0;
	sops[2].sem_flg = SEM_UNDO | IPC_NOWAIT;

	sops[3].sem_num = 1;
	sops[3].sem_op = 1;
	sops[3].sem_flg = SEM_UNDO | IPC_NOWAIT;

	ret = do_semop(id, sops, 4);
	if (ret < 0)
		return -ERRNO_TO_DSS_ERROR(errno);
	return 1;
}

static int mutex_try_lock(int id)
{
	struct sembuf sops[2];
	int ret;

	DSS_DEBUG_LOG("trying to lock\n");

	sops[0].sem_num = 0;
	sops[0].sem_op = 0;
	sops[0].sem_flg = SEM_UNDO | IPC_NOWAIT;

	sops[1].sem_num = 0;
	sops[1].sem_op = 1;
	sops[1].sem_flg = SEM_UNDO | IPC_NOWAIT;

	ret = do_semop(id, sops, 2);
	if (ret < 0)
		return -ERRNO_TO_DSS_ERROR(errno);
	return 1;
}

int lock_dss(char *config_file)
{
	int ret, key = get_key_or_die(config_file);

	ret = mutex_get(key, IPC_CREAT | 0600);
	if (ret < 0)
		return ret;
	return mutex_lock(ret);
}

int get_dss_pid(char *config_file, pid_t *pid)
{
	int ret, semid, key = get_key_or_die(config_file);

	ret = mutex_get(key, 0);
	if (ret < 0)
		return ret;
	semid = ret;
	ret = semctl(semid, 1, GETPID);
	if (ret < 0)
		return -E_NOT_RUNNING;
	*pid = ret;
	ret = mutex_try_lock(semid);
	if (ret >= 0)
		return -E_NOT_RUNNING;
	return 1;
}
