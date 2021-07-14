/**
 * Nanobenchmark: ADD
 *   JC. PROCESS = {ovewrite file and sync. at /test/$PROCESS}
 *       - TEST: journal commit
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define __STDC_FORMAT_MACROS

#include <inttypes.h>
#include <stdlib.h>
#include <assert.h>
#include "fxmark.h"
#include "util.h"

static int write_size = PAGE_SIZE;

static void set_test_root(struct worker *worker, char *test_root) {
	struct fx_opt *fx_opt = fx_opt_worker(worker);
	sprintf(test_root, "%s/%d", fx_opt->root, worker->id);
}

static int pre_work(struct worker *worker) {
	char *page = NULL;
	struct bench *bench = worker->bench;
	char test_root[PATH_MAX];
	int rc = 0;
	set_test_root(worker, test_root);

	if (posix_memalign((void **) &(worker->page), PAGE_SIZE, write_size))
		goto err_out;
	page = worker->page;
	if (!page)
		goto err_out;
	rc = mkdir_p(test_root);
	out:
	return rc;
	err_out:
	bench->stop = 1;
	rc = errno;
	free(page);
	goto out;
}

static int main_work(struct worker *worker) {
	char *page = worker->page;
	struct bench *bench = worker->bench;
	int rc = 0;
	uint64_t iter = 0;
	char test_root[PATH_MAX];
	set_test_root(worker, test_root);
	assert(page);


	if (bench->times) {
		for (iter = 0; !bench->stop && iter < bench->times; ++iter) {
			char file[PATH_MAX];
			int fd;
			/* create and close */
			snprintf(file, PATH_MAX, "%s/n_cwd-%lu.dat", test_root, iter);
			if ((fd = open(file, O_CREAT | O_RDWR, S_IRWXU)) == -1)
				goto err_out;
			if (pwrite(fd, page, write_size, 0) != write_size)
				goto err_out;
			if (fsync(fd) == -1)
				goto err_out;
			close(fd);
			if (unlink(file))
				goto err_out;
		}
	} else {
		for (iter = 0; !bench->stop; ++iter) {
			char file[PATH_MAX];
			int fd;
			/* create and close */
			snprintf(file, PATH_MAX, "%s/n_cwd-%lu.dat", test_root, iter);
			if ((fd = open(file, O_CREAT | O_RDWR, S_IRWXU)) == -1)
				goto err_out;
			if (pwrite(fd, page, write_size, 0) != write_size)
				goto err_out;
			if (fsync(fd) == -1)
				goto err_out;
			close(fd);
			if (unlink(file))
				goto err_out;
		}
	}
	out:
	worker->works = (double) iter;
	return rc;
	err_out:
	bench->stop = 1;
	rc = errno;
	goto out;
}

struct bench_operations n_cwd_ops = {
		.pre_work  = pre_work,
		.main_work = main_work,
};
