// lxj add
/**
 * For GC experiment
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
static int sync_period = 500;
static double file_size = 180; // in GB
static double data_size_each_loop = 72; // in GB

static void set_test_root(struct worker *worker, char *test_root) {
	struct fx_opt *fx_opt = fx_opt_worker(worker);
	sprintf(test_root, "%s/%d", fx_opt->root, worker->id);
}

static int prepare_phase_pre_work(struct worker *worker) {
	char *page = NULL;
	struct bench *bench = worker->bench;
	int cpu = bench->ncpu;
	char test_root[PATH_MAX];
	char file[PATH_MAX];
	int fd, rc = 0;

	double per_worker_file_size = (double) file_size / cpu;
	int write_loop = per_worker_file_size * 1024 * 1024 / 4;

	/* create test root */
	set_test_root(worker, test_root);
	rc = mkdir_p(test_root);
	if (rc) return rc;

	/* create a test file */
	snprintf(file, PATH_MAX, "%s/n_blk_wrt_pfsync.dat", test_root);
	if ((fd = open(file, O_CREAT | O_RDWR, S_IRWXU)) == -1)
		goto err_out;

	if (posix_memalign((void **) &(worker->page), PAGE_SIZE, write_size))
		goto err_out;
	page = worker->page;
	if (!page)
		goto err_out;

#if DEBUG
	/*to debug*/
	fprintf(stderr, "DEBUG: worker->id[%d], page address :%p\n",worker->id, page);
#endif

	/*set flag with O_DIRECT if necessary*/
	if (bench->directio && (fcntl(fd, F_SETFL, O_DIRECT) == -1))
		goto err_out;

	for (int i = 0; i < write_loop; i++) {
		if (write(fd, page, write_size) != write_size) // pre-write
			goto err_out;
	}

	if (fsync(fd) == -1)
		goto err_out;
	
	out:
	/* put fd to worker's private */
	worker->private[0] = (uint64_t) fd;
	return rc;
	err_out:
	bench->stop = 1;
	rc = errno;
	free(page);
	goto out;
}

static int prepare_phase_main_work(struct worker *worker) {
	int fd = (int) worker->private[0];
	worker->works = (double) 0;
	close(fd);
	return 0;
}

struct bench_operations n_blk_wrt_pfsync_ops_pre = {
		.pre_work  = prepare_phase_pre_work,
		.main_work = prepare_phase_main_work,
};

static int pre_work(struct worker *worker) {
	char *page = NULL;
	struct bench *bench = worker->bench;
	int cpu = bench->ncpu;
	char test_root[PATH_MAX];
	char file[PATH_MAX];
	int fd, rc = 0;

	/* create test root */
	set_test_root(worker, test_root);
	rc = mkdir_p(test_root);
	if (rc) return rc;

	/* create a test file */
	snprintf(file, PATH_MAX, "%s/n_blk_wrt_pfsync.dat", test_root);
	if ((fd = open(file, O_RDWR, S_IRWXU)) == -1)
		goto err_out;

	if (posix_memalign((void **) &(worker->page), PAGE_SIZE, write_size))
		goto err_out;
	page = worker->page;
	if (!page)
		goto err_out;

#if DEBUG
	/*to debug*/
	fprintf(stderr, "DEBUG: worker->id[%d], page address :%p\n",worker->id, page);
#endif

	/*set flag with O_DIRECT if necessary*/
	if (bench->directio && (fcntl(fd, F_SETFL, O_DIRECT) == -1))
		goto err_out;

	out:
	/* put fd to worker's private */
	worker->private[0] = (uint64_t) fd;
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
	int fd, rc = 0;
	uint64_t iter = 0;
	int pos;

	int nr_blocks = file_size * 1024 * 256 / bench->ncpu; 
	int total_loops = data_size_each_loop * 1024 * 256 / bench->ncpu; 

	srand((unsigned)time(NULL)); 
#if DEBUG
	fprintf(stderr, "DEBUG: worker->id[%d], main worker address :%p\n",
		worker->id, worker->page);
#endif
	assert(page);

	fd = (int) worker->private[0];
	if (bench->times)
		for (iter = 0; iter < total_loops; ++iter) { // add times limit
			pos = rand() % nr_blocks;  // random 
			// fprintf(stderr, "write pos %d\n", pos);
			if (pwrite(fd, page, write_size, pos) != write_size)
				goto err_out;
			if (iter % sync_period == 0) {
				if (fsync(fd) == -1)
					goto err_out;
				worker->works = (double) iter;
			}
		}
	else {
		for (iter = 0; !bench->stop; ++iter) { // no time limit
			pos = rand() % nr_blocks;  // random 
			if (pwrite(fd, page, write_size, pos) != write_size)
				goto err_out;
			if (iter % sync_period == 0) {
				if (fsync(fd) == -1)
					goto err_out;
				worker->works = (double) iter;
			}
		}
	}
	out:
	close(fd);
	worker->works = (double) iter;
	return rc;
	err_out:
	bench->stop = 1;
	rc = errno;
	free(page);
	goto out;
}

struct bench_operations n_blk_wrt_pfsync_ops = {
		.pre_work  = pre_work,
		.main_work = main_work,
};