#include <linux/atomic.h>
#include <linux/rwsem.h>
#include <linux/percpu.h>
#include <linux/wait.h>
#include <linux/lockdep.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/errno.h>

#include "rps.h"

int __rps_init_rwsem(struct rps *rps,
					 const char *name, struct lock_class_key *rw_sem_key) {
	rps->highway_cnt = alloc_percpu(int);
	if (unlikely(!rps->highway_cnt))
		return -ENOMEM;
	__init_rwsem(&rps->rw_sem, name, rw_sem_key);
	atomic_set(&rps->writers_cnt, 0);
	atomic_set(&rps->lowway_cnt, 0);
	init_waitqueue_head(&rps->writers_wait_q);
	return 0;
}

void rps_free_rwsem(struct rps *rps) {
	free_percpu(rps->highway_cnt);
	rps->highway_cnt = NULL;
}

static inline bool go_highway(struct rps *rps, int val) {
	bool highway = false;
	preempt_disable();
	if (likely(!atomic_read(&rps->writers_cnt))) {
		this_cpu_add(*rps->highway_cnt, val);
		highway = true;
	}
	preempt_enable();
	return highway;
}

static inline void go_lowway(struct rps *rps) {
	down_read(&rps->rw_sem);
	atomic_inc(&rps->lowway_cnt);
	up_read(&rps->rw_sem);
}

void rps_down_read(struct rps *rps) {
	if (likely(go_highway(rps, +1))) {
		return;
	}
	go_lowway(rps);
}

int rps_down_read_try_lock(struct rps *rps) {
	if (likely(go_highway(rps, +1))) {
		return 1;
	}
	if (down_read_trylock(&rps->rw_sem)) {
		atomic_inc(&rps->lowway_cnt);
		up_read(&rps->rw_sem);
		return 1;
	}
	return 0;
}

void rps_up_read(struct rps *rps) {
	if (likely(go_highway(rps, -1)))
		return;
	if (atomic_dec_and_test(&rps->lowway_cnt))
		wake_up_all(&rps->writers_wait_q);
}

static int clear_highway(struct rps *rps) {
	int sum = 0;
	int cpu;
	for_each_possible_cpu(cpu) {
		sum += per_cpu(*rps->highway_cnt, cpu);
		per_cpu(*rps->highway_cnt, cpu) = 0;
	}
	return sum;
}

void rps_down_write(struct rps *rps) {
	atomic_inc(&rps->writers_cnt); 
	synchronize_sched_expedited(); 
	down_write(&rps->rw_sem); 
	atomic_add(clear_highway(rps), &rps->lowway_cnt);
	wait_event(rps->writers_wait_q, !atomic_read(&rps->lowway_cnt)); 
}

void rps_up_write(struct rps *rps) {
	up_write(&rps->rw_sem); 
	synchronize_sched_expedited(); 
	atomic_dec(&rps->writers_cnt);
}
