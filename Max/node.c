/*
 * fs/f2fs/node.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/fs.h>
#include <linux/mpage.h>
#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/pagevec.h>
#include <linux/swap.h>

#include "f2fs.h"
#include "max_fs.h"
#include "node.h"
#include "segment.h"
#include "trace.h"
#include <trace/events/f2fs.h>

#define on_build_free_nids(nmi) mutex_is_locked(&nm_i->build_lock)
static struct kmem_cache *nat_entry_slab;
static struct kmem_cache *free_nid_slab;
static struct kmem_cache *nat_entry_set_slab;

#ifdef FILE_CELL
static struct kmem_cache *per_core_sets_pack_slab;
#endif

bool available_free_memory(struct f2fs_sb_info *sbi, int type) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct sysinfo val;
	unsigned long avail_ram;
	unsigned long mem_size = 0;
	bool res = false;

	si_meminfo(&val);

	/* only uses low memory */
	avail_ram = val.totalram - val.totalhigh;

	/*
	 * give 25%, 25%, 50%, 50%, 50% memory for each components respectively
	 */
	if (type == FREE_NIDS) {
#ifdef PER_CORE_NID_LIST
		nm_i->fcnt = sum_up_fcnt(nm_i);
#endif
		mem_size = (nm_i->fcnt * sizeof(struct free_nid)) >> PAGE_CACHE_SHIFT;
		res = mem_size < ((avail_ram * nm_i->ram_thresh / 100) >> 2);
	} else if (type == NAT_ENTRIES) {
#ifdef FILE_CELL
		int i;
		unsigned int count = 0;
		int nat_tree_cnt = nm_i->nat_tree_cnt;
		for (i = 0; i < nat_tree_cnt; i++) {
			count += nm_i->percore_nat_cnt[i];
		}
		nm_i->nat_cnt = count;
#endif
		mem_size = (nm_i->nat_cnt * sizeof(struct nat_entry)) >> PAGE_CACHE_SHIFT;
		res = mem_size < ((avail_ram * nm_i->ram_thresh / 100) >> 2);
	} else if (type == DIRTY_DENTS) {
		if (sbi->sb->s_bdi->wb.dirty_exceeded)
			return false;
		mem_size = get_pages(sbi, F2FS_DIRTY_DENTS);
		res = mem_size < ((avail_ram * nm_i->ram_thresh / 100) >> 1);
	} else if (type == INO_ENTRIES) {
		int i;
#ifdef FILE_CELL
		for (i = 0; i <= UPDATE_INO; i++) {
			mem_size += percpu_counter_sum_positive(&sbi->ino_mangement_num[i]) >> PAGE_CACHE_SHIFT;
		}
#else
		for (i = 0; i <= UPDATE_INO; i++)
			mem_size += (sbi->im[i].ino_num * sizeof(struct ino_entry)) >> PAGE_CACHE_SHIFT;
#endif
		res = mem_size < ((avail_ram * nm_i->ram_thresh / 100) >> 1);
	} else if (type == EXTENT_CACHE) {
		mem_size = (sbi->total_ext_tree * sizeof(struct extent_tree) +
					atomic_read(&sbi->total_ext_node) *
					sizeof(struct extent_node)) >> PAGE_CACHE_SHIFT;
		res = mem_size < ((avail_ram * nm_i->ram_thresh / 100) >> 1);
	} else {
		if (sbi->sb->s_bdi->wb.dirty_exceeded)
			return false;
	}
	return res;
}

static void clear_node_page_dirty(struct page *page) {
	struct address_space *mapping = page->mapping;
	unsigned int long flags;

	if (PageDirty(page)) {
		spin_lock_irqsave(&mapping->tree_lock, flags);
		radix_tree_tag_clear(&mapping->page_tree,
							 page_index(page),
							 PAGECACHE_TAG_DIRTY);
		spin_unlock_irqrestore(&mapping->tree_lock, flags);

		clear_page_dirty_for_io(page);
#ifdef FILE_CELL
		dec_dirty_node_page_count(F2FS_M_SB(mapping), NODE_IDX(nid_of_node(page), F2FS_M_SB(mapping)));
#else
		dec_page_count(F2FS_M_SB(mapping), F2FS_DIRTY_NODES);
#endif
	}
	ClearPageUptodate(page);
}

static struct page *get_current_nat_page(struct f2fs_sb_info *sbi, nid_t nid) {
	pgoff_t index = current_nat_addr(sbi, nid);
	return get_meta_page(sbi, index);
}

static struct page *get_next_nat_page(struct f2fs_sb_info *sbi, nid_t nid) {
	struct page *src_page;
	struct page *dst_page;
	pgoff_t src_off;
	pgoff_t dst_off;
	void *src_addr;
	void *dst_addr;
	struct f2fs_nm_info *nm_i = NM_I(sbi);

	src_off = current_nat_addr(sbi, nid);
	dst_off = next_nat_addr(sbi, src_off);

	/* get current nat block page with lock */
	src_page = get_meta_page(sbi, src_off);
	dst_page = grab_meta_page(sbi, dst_off);
#ifdef FILE_CELL
	// may scan the meta page multiple times during flush, so not a bug in per core NAT.
	f2fs_bug_on(sbi, PageDirty(src_page));
#else
	f2fs_bug_on(sbi, PageDirty(src_page));
#endif

	src_addr = page_address(src_page);
	dst_addr = page_address(dst_page);
	memcpy(dst_addr, src_addr, PAGE_CACHE_SIZE);
	set_page_dirty(dst_page);
	f2fs_put_page(src_page, 1);
#ifdef FILE_CELL
	set_to_next_nat(nm_i, nid);
#else
	set_to_next_nat(nm_i, nid);
#endif
	return dst_page;
}

static struct nat_entry *__lookup_nat_cache(struct f2fs_nm_info *nm_i, nid_t n) {
#ifdef FILE_CELL
	int tree_idx = TREE_IDX(n, nm_i);
	return radix_tree_lookup(&nm_i->nat_root[tree_idx], n);
#else
	return radix_tree_lookup(&nm_i->nat_root, n);
#endif
}

#ifdef FILE_CELL

static unsigned int __gang_lookup_nat_cache(struct f2fs_nm_info *nm_i, int tree_id,
											nid_t start, unsigned int nr, struct nat_entry **ep) {
	return radix_tree_gang_lookup(&nm_i->nat_root[tree_id], (void **) ep, start, nr);
}

#else
static unsigned int __gang_lookup_nat_cache(struct f2fs_nm_info *nm_i,
											nid_t start, unsigned int nr, struct nat_entry **ep) {
	return radix_tree_gang_lookup(&nm_i->nat_root, (void **) ep, start, nr);
}
#endif


static void __del_from_nat_cache(struct f2fs_nm_info *nm_i, struct nat_entry *e) {

#ifdef FILE_CELL
	list_del(&e->list);
	nid_t nid = nat_get_nid(e);
	int tree_idx = TREE_IDX(nid, nm_i);
	radix_tree_delete(&nm_i->nat_root[tree_idx], nid);
	nm_i->percore_nat_cnt[tree_idx]--;
#else
	list_del(&e->list);
	radix_tree_delete(&nm_i->nat_root, nat_get_nid(e));
	nm_i->nat_cnt--;
#endif
	kmem_cache_free(nat_entry_slab, e);
}

static void __set_nat_cache_dirty(struct f2fs_nm_info *nm_i,
								  struct nat_entry *ne) {
#ifdef FILE_CELL
	nid_t set = NAT_BLOCK_OFFSET(ne->ni.nid);
	struct nat_entry_set *head;

	if (get_nat_flag(ne, IS_DIRTY))
		return;
	nid_t nid = ne->ni.nid;
	int tree_idx = TREE_IDX(nid, nm_i);
	head = radix_tree_lookup(&nm_i->nat_set_root[tree_idx], set);
	if (!head) {
		head = f2fs_kmem_cache_alloc(nat_entry_set_slab, GFP_ATOMIC);
		INIT_LIST_HEAD(&head->entry_list);
		INIT_LIST_HEAD(&head->set_list);
		head->set = set;
		head->entry_cnt = 0;
		f2fs_radix_tree_insert(&nm_i->nat_set_root[tree_idx], set, head);
	}
	list_move_tail(&ne->list, &head->entry_list);
	nm_i->percore_dirty_nat_cnt[tree_idx]++;
	head->entry_cnt++;
	set_nat_flag(ne, IS_DIRTY, true);
#else
	nid_t set = NAT_BLOCK_OFFSET(ne->ni.nid);
	struct nat_entry_set *head;

	if (get_nat_flag(ne, IS_DIRTY))
		return;
	head = radix_tree_lookup(&nm_i->nat_set_root, set);
	if (!head) {
		head = f2fs_kmem_cache_alloc(nat_entry_set_slab, GFP_ATOMIC);
		INIT_LIST_HEAD(&head->entry_list);
		INIT_LIST_HEAD(&head->set_list);
		head->set = set;
		head->entry_cnt = 0;
		f2fs_radix_tree_insert(&nm_i->nat_set_root, set, head);
	}
	list_move_tail(&ne->list, &head->entry_list);
	nm_i->dirty_nat_cnt++;
	head->entry_cnt++;
	set_nat_flag(ne, IS_DIRTY, true);
#endif
}

static void __clear_nat_cache_dirty(struct f2fs_nm_info *nm_i,
									struct nat_entry *ne) {
	nid_t set = NAT_BLOCK_OFFSET(ne->ni.nid);
	struct nat_entry_set *head;
#ifdef FILE_CELL
	nid_t nid = ne->ni.nid;
	int tree_idx = TREE_IDX(nid, nm_i);
	head = radix_tree_lookup(&nm_i->nat_set_root[tree_idx], set);
	if (head) {
		list_move_tail(&ne->list, &nm_i->nat_entries[tree_idx]);
		set_nat_flag(ne, IS_DIRTY, false);
		head->entry_cnt--;
		nm_i->percore_dirty_nat_cnt[tree_idx]--;
	}
#else
	head = radix_tree_lookup(&nm_i->nat_set_root, set);
	if (head) {
		list_move_tail(&ne->list, &nm_i->nat_entries);
		set_nat_flag(ne, IS_DIRTY, false);
		head->entry_cnt--;
		nm_i->dirty_nat_cnt--;
	}
#endif
}

#ifdef FILE_CELL

static unsigned int __gang_lookup_nat_set(struct f2fs_nm_info *nm_i, int tree_idx,
										  nid_t start, unsigned int nr, struct nat_entry_set **ep) {
	return radix_tree_gang_lookup(&nm_i->nat_set_root[tree_idx], (void **) ep, start, nr);
}

#else
static unsigned int __gang_lookup_nat_set(struct f2fs_nm_info *nm_i,
										  nid_t start, unsigned int nr, struct nat_entry_set **ep) {

	return radix_tree_gang_lookup(&nm_i->nat_set_root, (void **) ep,
								  start, nr);
}
#endif


int need_dentry_mark(struct f2fs_sb_info *sbi, nid_t nid) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct nat_entry *e;
	bool need = false;

#ifdef FILE_CELL
	int tree_idx = TREE_IDX(nid, nm_i);
	down_read(&nm_i->nat_tree_lock[tree_idx]);
	e = __lookup_nat_cache(nm_i, nid);
	if (e) {
		if (!get_nat_flag(e, IS_CHECKPOINTED) &&
			!get_nat_flag(e, HAS_FSYNCED_INODE))
			need = true;
	}
	up_read(&nm_i->nat_tree_lock[tree_idx]);
#else
	down_read(&nm_i->nat_tree_lock);
	e = __lookup_nat_cache(nm_i, nid);
	if (e) {
		if (!get_nat_flag(e, IS_CHECKPOINTED) &&
			!get_nat_flag(e, HAS_FSYNCED_INODE))
			need = true;
	}
	up_read(&nm_i->nat_tree_lock);
#endif
	return need;
}

bool is_checkpointed_node(struct f2fs_sb_info *sbi, nid_t nid) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct nat_entry *e;
	bool is_cp = true;
#ifdef FILE_CELL
	int tree_idx = TREE_IDX(nid, nm_i);
	down_read(&nm_i->nat_tree_lock[tree_idx]);
	e = __lookup_nat_cache(nm_i, nid);
	if (e && !get_nat_flag(e, IS_CHECKPOINTED))
		is_cp = false;
	up_read(&nm_i->nat_tree_lock[tree_idx]);
#else
	down_read(&nm_i->nat_tree_lock);
	e = __lookup_nat_cache(nm_i, nid);
	if (e && !get_nat_flag(e, IS_CHECKPOINTED))
		is_cp = false;
	up_read(&nm_i->nat_tree_lock);
#endif
	return is_cp;
}

bool need_inode_block_update(struct f2fs_sb_info *sbi, nid_t ino) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct nat_entry *e;
	bool need_update = true;
#ifdef FILE_CELL
	int tree_idx = TREE_IDX(ino, nm_i);
	down_read(&nm_i->nat_tree_lock[tree_idx]);
	e = __lookup_nat_cache(nm_i, ino);
	if (e && get_nat_flag(e, HAS_LAST_FSYNC) &&
		(get_nat_flag(e, IS_CHECKPOINTED) ||
		 get_nat_flag(e, HAS_FSYNCED_INODE)))
		need_update = false;
	up_read(&nm_i->nat_tree_lock[tree_idx]);
#else
	down_read(&nm_i->nat_tree_lock);
	e = __lookup_nat_cache(nm_i, ino);
	if (e && get_nat_flag(e, HAS_LAST_FSYNC) &&
		(get_nat_flag(e, IS_CHECKPOINTED) ||
		 get_nat_flag(e, HAS_FSYNCED_INODE)))
		need_update = false;
	up_read(&nm_i->nat_tree_lock);
#endif
	return need_update;
}

static struct nat_entry *grab_nat_entry(struct f2fs_nm_info *nm_i, nid_t nid) {
	struct nat_entry *new;
	new = f2fs_kmem_cache_alloc(nat_entry_slab, GFP_ATOMIC);
#ifdef FILE_CELL
	int tree_idx = TREE_IDX(nid, nm_i);
	f2fs_radix_tree_insert(&nm_i->nat_root[tree_idx], nid, new);
	memset(new, 0, sizeof(struct nat_entry));
	nat_set_nid(new, nid);
	nat_reset_flag(new);
	list_add_tail(&new->list, &nm_i->nat_entries[tree_idx]);
	nm_i->percore_nat_cnt[tree_idx]++;
#else
	f2fs_radix_tree_insert(&nm_i->nat_root, nid, new);
	memset(new, 0, sizeof(struct nat_entry));
	nat_set_nid(new, nid);
	nat_reset_flag(new);
	list_add_tail(&new->list, &nm_i->nat_entries);
	nm_i->nat_cnt++;
#endif
	return new;
}

static void cache_nat_entry(struct f2fs_nm_info *nm_i, nid_t nid,
							struct f2fs_nat_entry *ne) {
	struct nat_entry *e;
#ifdef FILE_CELL
	int n = TREE_IDX(nid, nm_i);
	down_write(&nm_i->nat_tree_lock[n]);
	e = __lookup_nat_cache(nm_i, nid);
	if (!e) {
		e = grab_nat_entry(nm_i, nid);
		node_info_from_raw_nat(&e->ni, ne);
	}
	up_write(&nm_i->nat_tree_lock[n]);
#else
	down_write(&nm_i->nat_tree_lock);
	e = __lookup_nat_cache(nm_i, nid);
	if (!e) {
		e = grab_nat_entry(nm_i, nid);
		node_info_from_raw_nat(&e->ni, ne);
	}
	up_write(&nm_i->nat_tree_lock);
#endif
}

static void set_node_addr(struct f2fs_sb_info *sbi, struct node_info *ni,
						  block_t new_blkaddr, bool fsync_done) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct nat_entry *e;
#ifdef FILE_CELL
	int tree_idx = TREE_IDX(ni->nid, nm_i);
	down_write(&nm_i->nat_tree_lock[tree_idx]);
	e = __lookup_nat_cache(nm_i, ni->nid);
	if (!e) {
		e = grab_nat_entry(nm_i, ni->nid);
		copy_node_info(&e->ni, ni);
		f2fs_bug_on(sbi, ni->blk_addr == NEW_ADDR);
	} else if (new_blkaddr == NEW_ADDR) {
		/*
		 * when nid is reallocated,
		 * previous nat entry can be remained in nat cache.
		 * So, reinitialize it with new information.
		 */
		copy_node_info(&e->ni, ni);
		f2fs_bug_on(sbi, ni->blk_addr != NULL_ADDR);
	}

	/* sanity check */
	f2fs_bug_on(sbi, nat_get_blkaddr(e) != ni->blk_addr);
	f2fs_bug_on(sbi, nat_get_blkaddr(e) == NULL_ADDR &&
					 new_blkaddr == NULL_ADDR);
	f2fs_bug_on(sbi, nat_get_blkaddr(e) == NEW_ADDR &&
					 new_blkaddr == NEW_ADDR);
	f2fs_bug_on(sbi, nat_get_blkaddr(e) != NEW_ADDR &&
					 nat_get_blkaddr(e) != NULL_ADDR &&
					 new_blkaddr == NEW_ADDR);

	/* increment version no as node is removed */
	if (nat_get_blkaddr(e) != NEW_ADDR && new_blkaddr == NULL_ADDR) {
		unsigned char version = nat_get_version(e);
		nat_set_version(e, inc_node_version(version));
	}

	/* change address */
	nat_set_blkaddr(e, new_blkaddr);
	if (new_blkaddr == NEW_ADDR || new_blkaddr == NULL_ADDR)
		set_nat_flag(e, IS_CHECKPOINTED, false);
	__set_nat_cache_dirty(nm_i, e);

	/* update fsync_mark if its inode nat entry is still alive */
	if (ni->nid != ni->ino)
		e = __lookup_nat_cache(nm_i, ni->ino);
	if (e) {
		if (fsync_done && ni->nid == ni->ino)
			set_nat_flag(e, HAS_FSYNCED_INODE, true);
		set_nat_flag(e, HAS_LAST_FSYNC, fsync_done);
	}
	up_write(&nm_i->nat_tree_lock[tree_idx]);
#else
	down_write(&nm_i->nat_tree_lock);
	e = __lookup_nat_cache(nm_i, ni->nid);
	if (!e) {
		e = grab_nat_entry(nm_i, ni->nid);
		copy_node_info(&e->ni, ni);
		f2fs_bug_on(sbi, ni->blk_addr == NEW_ADDR);
	} else if (new_blkaddr == NEW_ADDR) {
		/*
		 * when nid is reallocated,
		 * previous nat entry can be remained in nat cache.
		 * So, reinitialize it with new information.
		 */
		copy_node_info(&e->ni, ni);
		f2fs_bug_on(sbi, ni->blk_addr != NULL_ADDR);
	}

	/* sanity check */
	f2fs_bug_on(sbi, nat_get_blkaddr(e) != ni->blk_addr);
	f2fs_bug_on(sbi, nat_get_blkaddr(e) == NULL_ADDR &&
					 new_blkaddr == NULL_ADDR);
	f2fs_bug_on(sbi, nat_get_blkaddr(e) == NEW_ADDR &&
					 new_blkaddr == NEW_ADDR);
	f2fs_bug_on(sbi, nat_get_blkaddr(e) != NEW_ADDR &&
					 nat_get_blkaddr(e) != NULL_ADDR &&
					 new_blkaddr == NEW_ADDR);

	/* increment version no as node is removed */
	if (nat_get_blkaddr(e) != NEW_ADDR && new_blkaddr == NULL_ADDR) {
		unsigned char version = nat_get_version(e);
		nat_set_version(e, inc_node_version(version));
	}

	/* change address */
	nat_set_blkaddr(e, new_blkaddr);
	if (new_blkaddr == NEW_ADDR || new_blkaddr == NULL_ADDR)
		set_nat_flag(e, IS_CHECKPOINTED, false);
	__set_nat_cache_dirty(nm_i, e);

	/* update fsync_mark if its inode nat entry is still alive */
	if (ni->nid != ni->ino)
		e = __lookup_nat_cache(nm_i, ni->ino);
	if (e) {
		if (fsync_done && ni->nid == ni->ino)
			set_nat_flag(e, HAS_FSYNCED_INODE, true);
		set_nat_flag(e, HAS_LAST_FSYNC, fsync_done);
	}
	up_write(&nm_i->nat_tree_lock);
#endif
}

int try_to_free_nats(struct f2fs_sb_info *sbi, int nr_shrink) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);

	if (available_free_memory(sbi, NAT_ENTRIES))
		return 0;
#ifdef FILE_CELL
	int nat_tree_cnt = nm_i->nat_tree_cnt;
	int divider = nat_tree_cnt;
	int i;
	for (i = 0; i < nat_tree_cnt; i++, divider--) {
		int nr_shrink_tmp = nr_shrink / divider;
		down_write(&nm_i->nat_tree_lock[i]);
		while (nr_shrink_tmp && !list_empty(&nm_i->nat_entries[i])) {
			struct nat_entry *ne;
			ne = list_first_entry(&nm_i->nat_entries[i], struct nat_entry, list);
			__del_from_nat_cache(nm_i, ne);
			nr_shrink--;
			nr_shrink_tmp--;
		}
		up_write(&nm_i->nat_tree_lock[i]);
	}
#else
	down_write(&nm_i->nat_tree_lock);
	while (nr_shrink && !list_empty(&nm_i->nat_entries)) {
		struct nat_entry *ne;
		ne = list_first_entry(&nm_i->nat_entries,
							  struct nat_entry, list);
		__del_from_nat_cache(nm_i, ne);
		nr_shrink--;
	}
	up_write(&nm_i->nat_tree_lock);
#endif
	return nr_shrink;
}

/*
 * This function always returns success
 */
void get_node_info(struct f2fs_sb_info *sbi, nid_t nid, struct node_info *ni) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_HOT_DATA); // only mlog 0 contains NAT journal
	struct f2fs_summary_block *sum = curseg->sum_blk;
	nid_t start_nid = START_NID(nid);
	struct f2fs_nat_block *nat_blk;
	struct page *page = NULL;
	struct f2fs_nat_entry ne;
	struct nat_entry *e;
	int i;

	ni->nid = nid;

	/* Check nat cache */
#ifdef FILE_CELL
	int tree_idx = TREE_IDX(nid, nm_i);
	down_read(&nm_i->nat_tree_lock[tree_idx]); // gains a read semaphore
	e = __lookup_nat_cache(nm_i, nid);
	if (e) {
		ni->ino = nat_get_ino(e);
		ni->blk_addr = nat_get_blkaddr(e);
		ni->version = nat_get_version(e);
	}
	up_read(&nm_i->nat_tree_lock[tree_idx]); // release a read semaphore
	if (e)
		return;
#else
	down_read(&nm_i->nat_tree_lock); // gains a read semaphore
	e = __lookup_nat_cache(nm_i, nid);
	if (e) {
		ni->ino = nat_get_ino(e);
		ni->blk_addr = nat_get_blkaddr(e);
		ni->version = nat_get_version(e);
	} else {
//		max_log("get_node_info, lookup nat cache, nid %u, not exist\n", nid);
	}
	up_read(&nm_i->nat_tree_lock); // release a read semaphore
	if (e)
		return;
#endif

	memset(&ne, 0, sizeof(struct f2fs_nat_entry));
	/* Check current segment summary */
	mutex_lock(&curseg->curseg_mutex);  
	i = lookup_journal_in_cursum(sum, NAT_JOURNAL, nid, 0);  // look up nat info in SSA area. i: index of journal entries
	if (i >= 0) {
		ne = nat_in_journal(sum, i);
		node_info_from_raw_nat(ni, &ne);
	}
	mutex_unlock(&curseg->curseg_mutex);
	if (i >= 0)
		goto cache;

	/* Fill node_info from nat page */
	page = get_current_nat_page(sbi, start_nid); // get locked meta page
	nat_blk = (struct f2fs_nat_block *) page_address(page);
	ne = nat_blk->entries[nid - start_nid];
	node_info_from_raw_nat(ni, &ne);
	f2fs_put_page(page, 1); // unlock meta page
	cache:
	/* cache nat entry */
	cache_nat_entry(NM_I(sbi), nid, &ne); // will gains a wrtite_semaphore on nat_tree_lock
}

/*
 * The maximum depth is four.
 * Offset[0] will have raw inode offset.
 */
static int get_node_path(struct f2fs_inode_info *fi, long block,
						 int offset[4], unsigned int noffset[4]) {
	const long direct_index = ADDRS_PER_INODE(fi);
	const long direct_blks = ADDRS_PER_BLOCK;
	const long dptrs_per_blk = NIDS_PER_BLOCK;
	const long indirect_blks = ADDRS_PER_BLOCK * NIDS_PER_BLOCK;
	const long dindirect_blks = indirect_blks * NIDS_PER_BLOCK;
	int n = 0;
	int level = 0;

	noffset[0] = 0;

	if (block < direct_index) {
		offset[n] = block;
		goto got;
	}
	block -= direct_index;
	if (block < direct_blks) {
		offset[n++] = NODE_DIR1_BLOCK;
		noffset[n] = 1;
		offset[n] = block;
		level = 1;
		goto got;
	}
	block -= direct_blks;
	if (block < direct_blks) {
		offset[n++] = NODE_DIR2_BLOCK;
		noffset[n] = 2;
		offset[n] = block;
		level = 1;
		goto got;
	}
	block -= direct_blks;
	if (block < indirect_blks) {
		offset[n++] = NODE_IND1_BLOCK;
		noffset[n] = 3;
		offset[n++] = block / direct_blks;
		noffset[n] = 4 + offset[n - 1];
		offset[n] = block % direct_blks;
		level = 2;
		goto got;
	}
	block -= indirect_blks;
	if (block < indirect_blks) {
		offset[n++] = NODE_IND2_BLOCK;
		noffset[n] = 4 + dptrs_per_blk;
		offset[n++] = block / direct_blks;
		noffset[n] = 5 + dptrs_per_blk + offset[n - 1];
		offset[n] = block % direct_blks;
		level = 2;
		goto got;
	}
	block -= indirect_blks;
	if (block < dindirect_blks) {
		offset[n++] = NODE_DIND_BLOCK;
		noffset[n] = 5 + (dptrs_per_blk * 2);
		offset[n++] = block / indirect_blks;
		noffset[n] = 6 + (dptrs_per_blk * 2) +
					 offset[n - 1] * (dptrs_per_blk + 1);
		offset[n++] = (block / direct_blks) % dptrs_per_blk;
		noffset[n] = 7 + (dptrs_per_blk * 2) +
					 offset[n - 2] * (dptrs_per_blk + 1) +
					 offset[n - 1];
		offset[n] = block % direct_blks;
		level = 3;
		goto got;
	} else {
		BUG();
	}
	got:
	return level;
}

/*
 * Caller should call f2fs_put_dnode(dn).
 * Also, it should grab and release a rwsem by calling f2fs_lock_op() and
 * f2fs_unlock_op() only if ro is not set RDONLY_NODE.
 * In the case of RDONLY_NODE, we don't need to care about mutex.
 */
int get_dnode_of_data(struct dnode_of_data *dn, pgoff_t index, int mode) {
	struct f2fs_sb_info *sbi = F2FS_I_SB(dn->inode);
	struct page *npage[4];
	struct page *parent = NULL;
	int offset[4];
	unsigned int noffset[4];
	nid_t nids[4];
	int level, i;
	int err = 0;

	/*    level 0 : addr[923-space(inline_xattrs&i_extra_size],  direct node[2], indirect node[2], double-indirect node[1]
	 *                               		                          ↓				↓					↓
	 *    level 1 :													addr[1018]   direct node[1018]  indirect node[1018]
	 *    																				↓					↓
	 *    level 2 :																	addr[1018]       direct node[1018]
	 *																										↓
	 *    level 3 :																						addr[1018]
	 */
	// offset: index offset in each level
	/*
	*
	*  Inode block (0)
	*    |- direct node (1)
	*    |- direct node (2)
	*    |- indirect node (3)
	*    |            `- direct node (4 => 4 + N - 1)
	*    |- indirect node (4 + N)
	*    |            `- direct node (5 + N => 5 + 2N - 1)
	*    `- double indirect node (5 + 2N)
	*                 `- indirect node (6 + 2N)
	*                       `- direct node
	*                 ......
	*                 `- indirect node ((6 + 2N) + x(N + 1))
	*                       `- direct node
	*                 ......
	*                 `- indirect node ((6 + 2N) + (N - 1)(N + 1))
	*                       `- direct node
	*/
	// noffset: node offset in each level, vertical view
	level = get_node_path(F2FS_I(dn->inode), index, offset, noffset);

	nids[0] = dn->inode->i_ino;
	npage[0] = dn->inode_page;

	if (!npage[0]) {
		npage[0] = get_node_page(sbi, nids[0]);
		if (IS_ERR(npage[0]))
			return PTR_ERR(npage[0]);
	}

	/* if inline_data is set, should not report any block indices */
	if (f2fs_has_inline_data(dn->inode) && index) {
		err = -ENOENT;
		f2fs_put_page(npage[0], 1);
		goto release_out;
	}

	parent = npage[0];
	if (level != 0)
		nids[1] = get_nid(parent, offset[0], true);
	dn->inode_page = npage[0];
	dn->inode_page_locked = true;

	/* get indirect or direct nodes */
	for (i = 1; i <= level; i++) {
		bool done = false;

		if (!nids[i] && mode == ALLOC_NODE) {
			/* alloc new node */
			if (!alloc_nid(sbi, &(nids[i]))) {
				err = -ENOSPC;
				goto release_pages;
			}

			dn->nid = nids[i];
			npage[i] = new_node_page(dn, noffset[i], NULL);
			if (IS_ERR(npage[i])) {
				alloc_nid_failed(sbi, nids[i]);
				err = PTR_ERR(npage[i]);
				goto release_pages;
			}

			set_nid(parent, offset[i - 1], nids[i], i == 1);
			alloc_nid_done(sbi, nids[i]);
			done = true;
		} else if (mode == LOOKUP_NODE_RA && i == level && level > 1) {
			npage[i] = get_node_page_ra(parent, offset[i - 1]);
			if (IS_ERR(npage[i])) {
				err = PTR_ERR(npage[i]);
				goto release_pages;
			}
			done = true;
		}
		if (i == 1) {
			dn->inode_page_locked = false;
			unlock_page(parent);
		} else {
			f2fs_put_page(parent, 1);
		}

		if (!done) {
			npage[i] = get_node_page(sbi, nids[i]);
			if (IS_ERR(npage[i])) {
				err = PTR_ERR(npage[i]);
				f2fs_put_page(npage[0], 0);
				goto release_out;
			}
		}
		if (i < level) {
			parent = npage[i];
			nids[i + 1] = get_nid(parent, offset[i], false);
		}
	}
	dn->nid = nids[level];
	dn->ofs_in_node = offset[level];
	dn->node_page = npage[level];
	dn->data_blkaddr = datablock_addr(dn->node_page, dn->ofs_in_node);
	return 0;

	release_pages:
	f2fs_put_page(parent, 1);
	if (i > 1)
		f2fs_put_page(npage[0], 0);
	release_out:
	dn->inode_page = NULL;
	dn->node_page = NULL;
	return err;
}

static void truncate_node(struct dnode_of_data *dn) {
	struct f2fs_sb_info *sbi = F2FS_I_SB(dn->inode);
	struct node_info ni;
	get_node_info(sbi, dn->nid, &ni);
	if (dn->inode->i_blocks == 0) {
		f2fs_bug_on(sbi, ni.blk_addr != NULL_ADDR);
		goto invalidate;
	}
	f2fs_bug_on(sbi, ni.blk_addr == NULL_ADDR);
	/* Deallocate node address */
	invalidate_blocks(sbi, ni.blk_addr);
	dec_valid_node_count(sbi, dn->inode);
	set_node_addr(sbi, &ni, NULL_ADDR, false);
	if (dn->nid == dn->inode->i_ino) {
		remove_orphan_inode(sbi, dn->nid);
		dec_valid_inode_count(sbi);
	} else {
		sync_inode_page(dn);
	}
	invalidate:
	clear_node_page_dirty(dn->node_page);
	set_sbi_flag(sbi, SBI_IS_DIRTY);
	f2fs_put_page(dn->node_page, 1);
#ifdef FILE_CELL
	invalidate_mapping_pages(NODE_MAPPING(sbi, dn->nid),
							 dn->node_page->index, dn->node_page->index);
#else
	invalidate_mapping_pages(NODE_MAPPING(sbi),
							 dn->node_page->index, dn->node_page->index);
#endif
	dn->node_page = NULL;
	trace_f2fs_truncate_node(dn->inode, dn->nid, ni.blk_addr);
}

static int truncate_dnode(struct dnode_of_data *dn) {
	struct page *page;

	if (dn->nid == 0)
		return 1;

	/* get direct node */
	page = get_node_page(F2FS_I_SB(dn->inode), dn->nid);
	if (IS_ERR(page) && PTR_ERR(page) == -ENOENT)
		return 1;
	else if (IS_ERR(page))
		return PTR_ERR(page);

	/* Make dnode_of_data for parameter */
	dn->node_page = page;
	dn->ofs_in_node = 0;
	truncate_data_blocks(dn);
	truncate_node(dn);
	return 1;
}

static int truncate_nodes(struct dnode_of_data *dn, unsigned int nofs,
						  int ofs, int depth) {
	struct dnode_of_data rdn = *dn;
	struct page *page;
	struct f2fs_node *rn;
	nid_t child_nid;
	unsigned int child_nofs;
	int freed = 0;
	int i, ret;

	if (dn->nid == 0)
		return NIDS_PER_BLOCK + 1;

	trace_f2fs_truncate_nodes_enter(dn->inode, dn->nid, dn->data_blkaddr);

	page = get_node_page(F2FS_I_SB(dn->inode), dn->nid);
	if (IS_ERR(page)) {
		trace_f2fs_truncate_nodes_exit(dn->inode, PTR_ERR(page));
		return PTR_ERR(page);
	}

	rn = F2FS_NODE(page);
	if (depth < 3) {
		for (i = ofs; i < NIDS_PER_BLOCK; i++, freed++) {
			child_nid = le32_to_cpu(rn->in.nid[i]);
			if (child_nid == 0)
				continue;
			rdn.nid = child_nid;
			ret = truncate_dnode(&rdn);
			if (ret < 0)
				goto out_err;
			set_nid(page, i, 0, false);
		}
	} else {
		child_nofs = nofs + ofs * (NIDS_PER_BLOCK + 1) + 1;
		for (i = ofs; i < NIDS_PER_BLOCK; i++) {
			child_nid = le32_to_cpu(rn->in.nid[i]);
			if (child_nid == 0) {
				child_nofs += NIDS_PER_BLOCK + 1;
				continue;
			}
			rdn.nid = child_nid;
			ret = truncate_nodes(&rdn, child_nofs, 0, depth - 1);
			if (ret == (NIDS_PER_BLOCK + 1)) {
				set_nid(page, i, 0, false);
				child_nofs += ret;
			} else if (ret < 0 && ret != -ENOENT) {
				goto out_err;
			}
		}
		freed = child_nofs;
	}

	if (!ofs) {
		/* remove current indirect node */
		dn->node_page = page;
		truncate_node(dn);
		freed++;
	} else {
		f2fs_put_page(page, 1);
	}
	trace_f2fs_truncate_nodes_exit(dn->inode, freed);
	return freed;

	out_err:
	f2fs_put_page(page, 1);
	trace_f2fs_truncate_nodes_exit(dn->inode, ret);
	return ret;
}

static int truncate_partial_nodes(struct dnode_of_data *dn,
								  struct f2fs_inode *ri, int *offset, int depth) {
	struct page *pages[2];
	nid_t nid[3];
	nid_t child_nid;
	int err = 0;
	int i;
	int idx = depth - 2;

	nid[0] = le32_to_cpu(ri->i_nid[offset[0] - NODE_DIR1_BLOCK]);
	if (!nid[0])
		return 0;

	/* get indirect nodes in the path */
	for (i = 0; i < idx + 1; i++) {
		/* reference count'll be increased */
		pages[i] = get_node_page(F2FS_I_SB(dn->inode), nid[i]);
		if (IS_ERR(pages[i])) {
			err = PTR_ERR(pages[i]);
			idx = i - 1;
			goto fail;
		}
		nid[i + 1] = get_nid(pages[i], offset[i + 1], false);
	}

	/* free direct nodes linked to a partial indirect node */
	for (i = offset[idx + 1]; i < NIDS_PER_BLOCK; i++) {
		child_nid = get_nid(pages[idx], i, false);
		if (!child_nid)
			continue;
		dn->nid = child_nid;
		err = truncate_dnode(dn);
		if (err < 0)
			goto fail;
		set_nid(pages[idx], i, 0, false);
	}

	if (offset[idx + 1] == 0) {
		dn->node_page = pages[idx];
		dn->nid = nid[idx];
		truncate_node(dn);
	} else {
		f2fs_put_page(pages[idx], 1);
	}
	offset[idx]++;
	offset[idx + 1] = 0;
	idx--;
	fail:
	for (i = idx; i >= 0; i--)
		f2fs_put_page(pages[i], 1);

	trace_f2fs_truncate_partial_nodes(dn->inode, nid, depth, err);

	return err;
}

/*
 * All the block addresses of data and nodes should be nullified.
 */
int truncate_inode_blocks(struct inode *inode, pgoff_t from) {
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	int err = 0, cont = 1;
	int level, offset[4], noffset[4];
	unsigned int nofs = 0;
	struct f2fs_inode *ri;
	struct dnode_of_data dn;
	struct page *page;

	trace_f2fs_truncate_inode_blocks_enter(inode, from);

	level = get_node_path(F2FS_I(inode), from, offset, noffset);
	restart:
	page = get_node_page(sbi, inode->i_ino);
	if (IS_ERR(page)) {
		trace_f2fs_truncate_inode_blocks_exit(inode, PTR_ERR(page));
		return PTR_ERR(page);
	}

	set_new_dnode(&dn, inode, page, NULL, 0);
	unlock_page(page);

	ri = F2FS_INODE(page);
	switch (level) {
		case 0:
		case 1:
			nofs = noffset[1];
			break;
		case 2:
			nofs = noffset[1];
			if (!offset[level - 1])
				goto skip_partial;
			err = truncate_partial_nodes(&dn, ri, offset, level);
			if (err < 0 && err != -ENOENT)
				goto fail;
			nofs += 1 + NIDS_PER_BLOCK;
			break;
		case 3:
			nofs = 5 + 2 * NIDS_PER_BLOCK;
			if (!offset[level - 1])
				goto skip_partial;
			err = truncate_partial_nodes(&dn, ri, offset, level);
			if (err < 0 && err != -ENOENT)
				goto fail;
			break;
		default:
			BUG();
	}

	skip_partial:
	while (cont) {
		dn.nid = le32_to_cpu(ri->i_nid[offset[0] - NODE_DIR1_BLOCK]);
		switch (offset[0]) {
			case NODE_DIR1_BLOCK:
			case NODE_DIR2_BLOCK:
				err = truncate_dnode(&dn);
				break;

			case NODE_IND1_BLOCK:
			case NODE_IND2_BLOCK:
				err = truncate_nodes(&dn, nofs, offset[1], 2);
				break;

			case NODE_DIND_BLOCK:
				err = truncate_nodes(&dn, nofs, offset[1], 3);
				cont = 0;
				break;

			default:
				BUG();
		}
		if (err < 0 && err != -ENOENT)
			goto fail;
		if (offset[1] == 0 &&
			ri->i_nid[offset[0] - NODE_DIR1_BLOCK]) {
			lock_page(page);
#ifdef FILE_CELL
			if (unlikely(page->mapping != NODE_MAPPING(sbi, inode->i_ino))) {
//				printk("restart truncate_inode_blocks\n");
				f2fs_put_page(page, 1);
				goto restart;
			}
#else
			if (unlikely(page->mapping != NODE_MAPPING(sbi))) {
				f2fs_put_page(page, 1);
				goto restart;
			}
#endif
			f2fs_wait_on_page_writeback(page, NODE);
			ri->i_nid[offset[0] - NODE_DIR1_BLOCK] = 0;
			set_page_dirty(page);
			unlock_page(page);
		}
		offset[1] = 0;
		offset[0]++;
		nofs += err;
	}
	fail:
	f2fs_put_page(page, 0);
	trace_f2fs_truncate_inode_blocks_exit(inode, err);
	return err > 0 ? 0 : err;
}

int truncate_xattr_node(struct inode *inode, struct page *page) {
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	nid_t nid = F2FS_I(inode)->i_xattr_nid;
	struct dnode_of_data dn;
	struct page *npage;

	if (!nid)
		return 0;

	npage = get_node_page(sbi, nid);
	if (IS_ERR(npage))
		return PTR_ERR(npage);

	F2FS_I(inode)->i_xattr_nid = 0;

	/* need to do checkpoint during fsync */
	F2FS_I(inode)->xattr_ver = cur_cp_version(F2FS_CKPT(sbi));

	set_new_dnode(&dn, inode, page, npage, nid);

	if (page)
		dn.inode_page_locked = true;
	truncate_node(&dn);
	return 0;
}

/*
 * Caller should grab and release a rwsem by calling f2fs_lock_op() and
 * f2fs_unlock_op().
 */
void remove_inode_page(struct inode *inode) {
	struct dnode_of_data dn;

	set_new_dnode(&dn, inode, NULL, NULL, inode->i_ino);
	if (get_dnode_of_data(&dn, 0, LOOKUP_NODE))
		return;

	if (truncate_xattr_node(inode, dn.inode_page)) {
		f2fs_put_dnode(&dn);
		return;
	}

	/* remove potential inline_data blocks */
	if (S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
		S_ISLNK(inode->i_mode))
		truncate_data_blocks_range(&dn, 1);

	/* 0 is possible, after f2fs_new_inode() has failed */
	f2fs_bug_on(F2FS_I_SB(inode),
				inode->i_blocks != 0 && inode->i_blocks != 1);

	/* will put inode & node pages */
	truncate_node(&dn);
}

struct page *new_inode_page(struct inode *inode) {
	struct dnode_of_data dn;

	/* allocate inode page for new inode */
	set_new_dnode(&dn, inode, NULL, NULL, inode->i_ino);

	/* caller should f2fs_put_page(page, 1); */
	return new_node_page(&dn, 0, NULL);
}

struct page *new_node_page(struct dnode_of_data *dn,
						   unsigned int ofs, struct page *ipage) {
	struct f2fs_sb_info *sbi = F2FS_I_SB(dn->inode);
	struct node_info old_ni, new_ni;
	struct page *page;
	int err;

	if (unlikely(is_inode_flag_set(F2FS_I(dn->inode), FI_NO_ALLOC)))
		return ERR_PTR(-EPERM);
#ifdef FILE_CELL
	// printk(KERN_INFO"on node inode %d\n",  dn->nid % sbi->node_count);
	page = grab_cache_page(NODE_MAPPING(sbi, dn->nid), dn->nid);
#else
	page = grab_cache_page(NODE_MAPPING(sbi), dn->nid);
#endif
	if (!page)
		return ERR_PTR(-ENOMEM);

	if (unlikely(!inc_valid_node_count(sbi, dn->inode))) {
		err = -ENOSPC;
		goto fail;
	}

	get_node_info(sbi, dn->nid, &old_ni);

	/* Reinitialize old_ni with new node page */
	if (old_ni.blk_addr != NULL_ADDR) {
		printk("bug on new node page nid:%d, blkaddr:%d\n", old_ni.nid, old_ni.blk_addr);
		f2fs_bug_on(sbi, old_ni.blk_addr != NULL_ADDR);
	}
	new_ni = old_ni;
	new_ni.ino = dn->inode->i_ino;
	set_node_addr(sbi, &new_ni, NEW_ADDR, false);

	f2fs_wait_on_page_writeback(page, NODE);
	fill_node_footer(page, dn->nid, dn->inode->i_ino, ofs, true);
	set_cold_node(dn->inode, page);
	SetPageUptodate(page);
	set_page_dirty(page);

	if (f2fs_has_xattr_block(ofs))
		F2FS_I(dn->inode)->i_xattr_nid = dn->nid;

	dn->node_page = page;
	if (ipage)
		update_inode(dn->inode, ipage);
	else
		sync_inode_page(dn);
	if (ofs == 0)
		inc_valid_inode_count(sbi);

	return page;

	fail:
	clear_node_page_dirty(page);
	f2fs_put_page(page, 1);
	return ERR_PTR(err);
}

/*
 * Caller should do after getting the following values.
 * 0: f2fs_put_page(page, 0)
 * LOCKED_PAGE: f2fs_put_page(page, 1)
 * error: nothing
 */
static int read_node_page(struct page *page, int rw) {
	struct f2fs_sb_info *sbi = F2FS_P_SB(page);
	struct node_info ni;
	struct f2fs_io_info fio = {
			.sbi = sbi,
			.type = NODE,
			.rw = rw,
			.page = page,
			.encrypted_page = NULL,
	};

	if (PageUptodate(page))
		return LOCKED_PAGE;

	get_node_info(sbi, page->index, &ni);

	if (unlikely(ni.blk_addr == NULL_ADDR)) {
		ClearPageUptodate(page);
		f2fs_put_page(page, 1);
		return -ENOENT;
	}

	fio.blk_addr = ni.blk_addr;
	return f2fs_submit_page_bio(
			&fio); // read from ssd, fill the locked page with data located in blk_address
}

/*
 * Readahead a node page
 */
void ra_node_page(struct f2fs_sb_info *sbi, nid_t nid) {
	struct page *apage;
	int err;
#ifdef FILE_CELL
	apage = find_get_page(NODE_MAPPING(sbi, nid), nid);
#else
	apage = find_get_page(NODE_MAPPING(sbi), nid);
#endif
	if (apage && PageUptodate(apage)) {
		f2fs_put_page(apage, 0);
		return;
	}
	f2fs_put_page(apage, 0);
#ifdef FILE_CELL
	apage = grab_cache_page(NODE_MAPPING(sbi, nid), nid);
#else
	apage = grab_cache_page(NODE_MAPPING(sbi), nid);
#endif
	if (!apage)
		return;

	err = read_node_page(apage, READA);
	if (err == 0)
		f2fs_put_page(apage, 0);
	else if (err == LOCKED_PAGE)
		f2fs_put_page(apage, 1);
}

struct page *get_node_page(struct f2fs_sb_info *sbi, pgoff_t nid) {
	struct page *page;
	int err;
	repeat:
#ifdef FILE_CELL
	page = grab_cache_page(NODE_MAPPING(sbi, nid), nid); // find or create a locked page
#else
	page = grab_cache_page(NODE_MAPPING(sbi), nid); // find or create a locked page
#endif
	if (!page)
		return ERR_PTR(-ENOMEM);

	err = read_node_page(page, READ_SYNC);
	if (err < 0)
		return ERR_PTR(err);
	else if (err != LOCKED_PAGE)
		lock_page(page);

	if (unlikely(!PageUptodate(page) || nid != nid_of_node(page))) {
		ClearPageUptodate(page);
		f2fs_put_page(page, 1);
		return ERR_PTR(-EIO);
	}
#ifdef FILE_CELL
	if (unlikely(page->mapping != NODE_MAPPING(sbi, nid))) {
//		printk("get node page repeat \n");
		f2fs_put_page(page, 1);
		goto repeat;
	}
#else
	if (unlikely(page->mapping != NODE_MAPPING(sbi))) {
		f2fs_put_page(page, 1);
		goto repeat;
	}
#endif
	return page;
}

/*
 * Return a locked page for the desired node page.
 * And, readahead MAX_RA_NODE number of node pages.
 */
struct page *get_node_page_ra(struct page *parent, int start) {
	struct f2fs_sb_info *sbi = F2FS_P_SB(parent);
	struct blk_plug plug;
	struct page *page;
	int err, i, end;
	nid_t nid;

	/* First, try getting the desired direct node. */
	nid = get_nid(parent, start, false);
	if (!nid)
		return ERR_PTR(-ENOENT);
	repeat:
#ifdef FILE_CELL
	page = grab_cache_page(NODE_MAPPING(sbi, nid), nid);
#else
	page = grab_cache_page(NODE_MAPPING(sbi), nid);
#endif
	if (!page)
		return ERR_PTR(-ENOMEM);

	err = read_node_page(page, READ_SYNC);
	if (err < 0)
		return ERR_PTR(err);
	else if (err == LOCKED_PAGE)
		goto page_hit;

	blk_start_plug(&plug);

	/* Then, try readahead for siblings of the desired node */
	end = start + MAX_RA_NODE;
	end = min(end, NIDS_PER_BLOCK);
	for (i = start + 1; i < end; i++) {
		nid = get_nid(parent, i, false);
		if (!nid)
			continue;
		ra_node_page(sbi, nid);
	}

	blk_finish_plug(&plug);

	lock_page(page);
#ifdef FILE_CELL
	if (unlikely(page->mapping != NODE_MAPPING(sbi, nid))) {
//		printk("get node page ra repeat\n");
		f2fs_put_page(page, 1);
		goto repeat;
	}
#else
	if (unlikely(page->mapping != NODE_MAPPING(sbi))) {
		f2fs_put_page(page, 1);
		goto repeat;
	}
#endif
	page_hit:
	if (unlikely(!PageUptodate(page))) {
		f2fs_put_page(page, 1);
		return ERR_PTR(-EIO);
	}
	return page;
}

void sync_inode_page(struct dnode_of_data *dn) {
	if (IS_INODE(dn->node_page) || dn->inode_page == dn->node_page) {
		update_inode(dn->inode, dn->node_page);
	} else if (dn->inode_page) {
		if (!dn->inode_page_locked)
			lock_page(dn->inode_page);
		update_inode(dn->inode, dn->inode_page);
		if (!dn->inode_page_locked)
			unlock_page(dn->inode_page);
	} else {
		update_inode_page(dn->inode);
	}
}

#ifdef FILE_CELL

int sync_node_pages(struct f2fs_sb_info *sbi, nid_t ino, nid_t node_idx,
					struct writeback_control *wbc) {
	pgoff_t index, end;
	struct pagevec pvec;
	int step = ino ? 2 : 0;  // 0: sync all, 2: fsync on file dnodes
	int nwritten = 0, wrote = 0;

	pagevec_init(&pvec, 0);

	next_step:
	index = 0;
	end = LONG_MAX;

	while (index <= end) {
		int i, nr_pages;
		nr_pages = pagevec_lookup_tag(&pvec, NODE_MAPPING(sbi, node_idx), &index, PAGECACHE_TAG_DIRTY,
									  min(end - index, (pgoff_t) PAGEVEC_SIZE - 1) + 1);
		if (nr_pages == 0)
			break;

		for (i = 0; i < nr_pages; i++) {
			struct page *page = pvec.pages[i];

			/*
			 * flushing sequence with step:
			 * 0. indirect nodes
			 * 1. dentry dnodes
			 * 2. file dnodes
			 */
			if (step == 0 && IS_DNODE(page))
				continue;
			if (step == 1 && (!IS_DNODE(page) ||
							  is_cold_node(page)))
				continue;
			if (step == 2 && (!IS_DNODE(page) ||
							  !is_cold_node(page)))
				continue;

			/*
			 * If an fsync mode,
			 * we should not skip writing node pages.
			 */
			if (ino && ino_of_node(page) == ino)
				lock_page(page);
			else if (!trylock_page(page))
				continue;

			if (unlikely(page->mapping != NODE_MAPPING(sbi, node_idx))) {
				continue_unlock:
				unlock_page(page);
				continue;
			}
			if (ino && ino_of_node(page) != ino)
				goto continue_unlock;

			if (!PageDirty(page)) {
				/* someone wrote it for us */
				goto continue_unlock;
			}

			if (!clear_page_dirty_for_io(page))
				goto continue_unlock;

			/* called by fsync() */
			if (ino && IS_DNODE(page)) {
				set_fsync_mark(page, 1);
				if (IS_INODE(page))
					set_dentry_mark(page,
									need_dentry_mark(sbi, ino));
				nwritten++;
			} else {
				set_fsync_mark(page, 0);
				set_dentry_mark(page, 0);
			}

			if (NODE_MAPPING(sbi, node_idx)->a_ops->writepage(page, wbc))
				unlock_page(page);
			else
				wrote++;

			if (--wbc->nr_to_write == 0)
				break;
		}
		pagevec_release(&pvec);
		cond_resched();

		if (wbc->nr_to_write == 0) {
			step = 2;
			break;
		}
	}

	if (step < 2) {
		step++;
		goto next_step;
	}

	if (wrote)
		f2fs_submit_merged_bio(sbi, NODE, WRITE);
	return nwritten;
}

#else

int sync_node_pages(struct f2fs_sb_info *sbi, nid_t ino,
					struct writeback_control *wbc) {
	pgoff_t index, end;
	struct pagevec pvec;
	int step = ino ? 2 : 0;
	int nwritten = 0, wrote = 0;

	pagevec_init(&pvec, 0);

	next_step:
	index = 0;
	end = LONG_MAX;

	while (index <= end) {
		int i, nr_pages;
		nr_pages = pagevec_lookup_tag(&pvec, NODE_MAPPING(sbi), &index,
									  PAGECACHE_TAG_DIRTY,
									  min(end - index, (pgoff_t) PAGEVEC_SIZE - 1) + 1);
		if (nr_pages == 0)
			break;

		for (i = 0; i < nr_pages; i++) {
			struct page *page = pvec.pages[i];

			/*
			 * flushing sequence with step:
			 * 0. indirect nodes
			 * 1. dentry dnodes
			 * 2. file dnodes
			 */
			if (step == 0 && IS_DNODE(page))
				continue;
			if (step == 1 && (!IS_DNODE(page) ||
							  is_cold_node(page)))
				continue;
			if (step == 2 && (!IS_DNODE(page) ||
							  !is_cold_node(page)))
				continue;

			/*
			 * If an fsync mode,
			 * we should not skip writing node pages.
			 */
			if (ino && ino_of_node(page) == ino)
				lock_page(page);
			else if (!trylock_page(page))
				continue;

			if (unlikely(page->mapping != NODE_MAPPING(sbi))) {
				continue_unlock:
				unlock_page(page);
				continue;
			}
			if (ino && ino_of_node(page) != ino)
				goto continue_unlock;

			if (!PageDirty(page)) {
				/* someone wrote it for us */
				goto continue_unlock;
			}

			if (!clear_page_dirty_for_io(page))
				goto continue_unlock;

			/* called by fsync() */
			if (ino && IS_DNODE(page)) {
				set_fsync_mark(page, 1);
				if (IS_INODE(page))
					set_dentry_mark(page,
									need_dentry_mark(sbi, ino));
				nwritten++;
			} else {
				set_fsync_mark(page, 0);
				set_dentry_mark(page, 0);
			}

			if (NODE_MAPPING(sbi)->a_ops->writepage(page, wbc))
				unlock_page(page);
			else
				wrote++;

			if (--wbc->nr_to_write == 0)
				break;
		}
		pagevec_release(&pvec);
		cond_resched();

		if (wbc->nr_to_write == 0) {
			step = 2;
			break;
		}
	}

	if (step < 2) {
		step++;
		goto next_step;
	}

	if (wrote)
		f2fs_submit_merged_bio(sbi, NODE, WRITE);
	return nwritten;
}

#endif


int wait_on_node_pages_writeback(struct f2fs_sb_info *sbi, nid_t ino) {
	pgoff_t index = 0, end = LONG_MAX;
	struct pagevec pvec;
	int ret2 = 0, ret = 0;

	pagevec_init(&pvec, 0);

	while (index <= end) {
		int i, nr_pages;
#ifdef FILE_CELL
		nr_pages = pagevec_lookup_tag(&pvec, NODE_MAPPING(sbi, ino), &index,
									  PAGECACHE_TAG_WRITEBACK,
									  min(end - index, (pgoff_t) PAGEVEC_SIZE - 1) + 1);
#else
		nr_pages = pagevec_lookup_tag(&pvec, NODE_MAPPING(sbi), &index,
									  PAGECACHE_TAG_WRITEBACK,
									  min(end - index, (pgoff_t) PAGEVEC_SIZE - 1) + 1);
#endif
		if (nr_pages == 0)
			break;

		for (i = 0; i < nr_pages; i++) {
			struct page *page = pvec.pages[i];

			/* until radix tree lookup accepts end_index */
			if (unlikely(page->index > end))
				continue;

			if (ino && ino_of_node(page) == ino) {
				f2fs_wait_on_page_writeback(page, NODE);
				if (TestClearPageError(page))
					ret = -EIO;
			}
		}
		pagevec_release(&pvec);
		cond_resched();
	}

#ifdef FILE_CELL
	if (unlikely(test_and_clear_bit(AS_ENOSPC, &NODE_MAPPING(sbi, ino)->flags)))
		ret2 = -ENOSPC;
	if (unlikely(test_and_clear_bit(AS_EIO, &NODE_MAPPING(sbi, ino)->flags)))
		ret2 = -EIO;
#else
	if (unlikely(test_and_clear_bit(AS_ENOSPC, &NODE_MAPPING(sbi)->flags)))
		ret2 = -ENOSPC;
	if (unlikely(test_and_clear_bit(AS_EIO, &NODE_MAPPING(sbi)->flags)))
		ret2 = -EIO;
#endif
	if (!ret)
	if (!ret)
		ret = ret2;
	return ret;
}

static int f2fs_write_node_page(struct page *page,
								struct writeback_control *wbc) {
	struct f2fs_sb_info *sbi = F2FS_P_SB(page);
	nid_t nid;
	struct node_info ni;
	struct f2fs_io_info fio = {
			.sbi = sbi,
			.type = NODE,
			.rw = (wbc->sync_mode == WB_SYNC_ALL) ? WRITE_SYNC : WRITE,
			.page = page,
			.encrypted_page = NULL,
	};

	trace_f2fs_writepage(page, NODE);

	if (unlikely(is_sbi_flag_set(sbi, SBI_POR_DOING)))
		goto redirty_out;
	if (unlikely(f2fs_cp_error(sbi)))
		goto redirty_out;

	f2fs_wait_on_page_writeback(page, NODE);

	/* get old block addr of this node page */
	nid = nid_of_node(page);
	f2fs_bug_on(sbi, page->index != nid);

	get_node_info(sbi, nid, &ni);

	/* This page is already truncated */
	if (unlikely(ni.blk_addr == NULL_ADDR)) {
		ClearPageUptodate(page);
#ifdef FILE_CELL
		dec_dirty_node_page_count(sbi, NODE_IDX(nid, sbi));
#else
		dec_page_count(sbi, F2FS_DIRTY_NODES);
#endif
		unlock_page(page);
		return 0;
	}
#ifdef RPS
	if (wbc->for_reclaim) {
		if (!rps_down_read_try_lock(&sbi->max_info->rps_node_write))
			goto redirty_out;
	} else {
		rps_down_read(&sbi->max_info->rps_node_write);
	}
#else
	if (wbc->for_reclaim) {
		if (!down_read_trylock(&sbi->node_write))
			goto redirty_out;
	} else {
		down_read(&sbi->node_write);
	}
#endif

	set_page_writeback(page);
	fio.blk_addr = ni.blk_addr;
	write_node_page(nid, &fio);
	set_node_addr(sbi, &ni, fio.blk_addr, is_fsync_dnode(page));
#ifdef FILE_CELL
	dec_dirty_node_page_count(sbi, NODE_IDX(nid, sbi));
#else
	dec_page_count(sbi, F2FS_DIRTY_NODES);
#endif
#ifdef RPS
	rps_up_read(&sbi->max_info->rps_node_write);
#else
	up_read(&sbi->node_write);
#endif
	unlock_page(page);

	if (wbc->for_reclaim)
		f2fs_submit_merged_bio(sbi, NODE, WRITE);

	return 0;

	redirty_out:
	redirty_page_for_writepage(wbc, page);
	return AOP_WRITEPAGE_ACTIVATE;
}

static int f2fs_write_node_pages(struct address_space *mapping,
								 struct writeback_control *wbc) {
	struct f2fs_sb_info *sbi = F2FS_M_SB(mapping);
	long diff;

	trace_f2fs_writepages(mapping->host, wbc, NODE);
	/* balancing f2fs's metadata in background */
	f2fs_balance_fs_bg(sbi);
	/* collect a number of dirty node pages and write together */
#ifdef FILE_CELL
	if (get_dirty_node_pages(sbi, mapping->host->i_ino - F2FS_NODE_INO(sbi)) < nr_pages_to_skip(sbi, NODE)) {
		goto skip_write;
	}
#else
	if (get_pages(sbi, F2FS_DIRTY_NODES) < nr_pages_to_skip(sbi, NODE)) {
		goto skip_write;
	}
#endif
	diff = nr_pages_to_write(sbi, NODE, wbc);
	wbc->sync_mode = WB_SYNC_NONE;
#ifdef FILE_CELL
	f2fs_bug_on(sbi, mapping->host->i_ino - F2FS_NODE_INO(sbi) >= sbi->node_count);
	f2fs_bug_on(sbi, mapping->host->i_ino - F2FS_NODE_INO(sbi) < 0);
	sync_node_pages(sbi, 0, mapping->host->i_ino - F2FS_NODE_INO(sbi), wbc);
#else
	sync_node_pages(sbi, 0, wbc);
#endif
	wbc->nr_to_write = max((long) 0, wbc->nr_to_write - diff);
	return 0;
	skip_write:
#ifdef FILE_CELL
	wbc->pages_skipped += get_dirty_node_pages(sbi, mapping->host->i_ino - F2FS_NODE_INO(sbi));
#else
	wbc->pages_skipped += get_pages(sbi, F2FS_DIRTY_NODES);
#endif
	return 0;
}

static int f2fs_set_node_page_dirty(struct page *page) {
	trace_f2fs_set_page_dirty(page, NODE);

	SetPageUptodate(page);
	if (!PageDirty(page)) {
		__set_page_dirty_nobuffers(page);
#ifdef FILE_CELL
		inc_dirty_node_page_count(F2FS_P_SB(page), NODE_IDX(nid_of_node(page), F2FS_P_SB(page)));
#else
		inc_page_count(F2FS_P_SB(page), F2FS_DIRTY_NODES);
#endif
		SetPagePrivate(page);
		f2fs_trace_pid(page);
		return 1;
	}
	return 0;
}

/*
 * Structure of the f2fs node operations
 */
const struct address_space_operations f2fs_node_aops = {
		.writepage    = f2fs_write_node_page,
		.writepages    = f2fs_write_node_pages,
		.set_page_dirty    = f2fs_set_node_page_dirty,
		.invalidatepage    = f2fs_invalidate_page,
		.releasepage    = f2fs_release_page,
};

static struct free_nid *__lookup_free_nid_list(struct f2fs_nm_info *nm_i,
											   nid_t n) {
#ifdef PER_CORE_NID_LIST
	int list_idx = LIST_IDX(n, nm_i);
	return radix_tree_lookup(&nm_i->free_nid_root[list_idx], n);
#else
	return radix_tree_lookup(&nm_i->free_nid_root, n);
#endif
}

static void __del_from_free_nid_list(struct f2fs_nm_info *nm_i,
									 struct free_nid *i) {
	list_del(&i->list);
#ifdef PER_CORE_NID_LIST
	int list_idx = LIST_IDX(i->nid, nm_i);
	radix_tree_delete(&nm_i->free_nid_root[list_idx], i->nid);
#else
	radix_tree_delete(&nm_i->free_nid_root, i->nid);
#endif
}

static int add_free_nid(struct f2fs_sb_info *sbi, nid_t nid, bool build) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct free_nid *i;
	struct nat_entry *ne;
	bool allocated = false;

	if (!available_free_memory(sbi, FREE_NIDS)) {
		return -1;
	}

	/* 0 nid should not be used */
	if (unlikely(nid == 0))
		return 0;
	if (build) {
		/* do not add allocated nids */
#ifdef FILE_CELL
		int tree_idx = TREE_IDX(nid, nm_i);
		down_read(&nm_i->nat_tree_lock[tree_idx]);
		ne = __lookup_nat_cache(nm_i, nid);
		if (ne && (!get_nat_flag(ne, IS_CHECKPOINTED) ||
				   nat_get_blkaddr(ne) != NULL_ADDR))
			allocated = true;
		up_read(&nm_i->nat_tree_lock[tree_idx]);
#else
		down_read(&nm_i->nat_tree_lock);
		ne = __lookup_nat_cache(nm_i, nid);
		if (ne &&
			(!get_nat_flag(ne, IS_CHECKPOINTED) ||
			 nat_get_blkaddr(ne) != NULL_ADDR))
			allocated = true;
		up_read(&nm_i->nat_tree_lock);
#endif
		if (allocated)
			return 0;
	}

	i = f2fs_kmem_cache_alloc(free_nid_slab, GFP_NOFS);
	i->nid = nid;
	i->state = NID_NEW;

	if (radix_tree_preload(GFP_NOFS)) {
		kmem_cache_free(free_nid_slab, i);
		return 0;
	}

#ifdef PER_CORE_NID_LIST
	int list_idx = LIST_IDX(nid, nm_i);
	spin_lock(&nm_i->free_nid_list_lock[list_idx]);
	if (radix_tree_insert(&nm_i->free_nid_root[list_idx], i->nid, i)) {
		spin_unlock(&nm_i->free_nid_list_lock[list_idx]);
		radix_tree_preload_end();
		kmem_cache_free(free_nid_slab, i);
		return 0;
	}
	list_add_tail(&i->list, &nm_i->free_nid_list[list_idx]);
	nm_i->percore_fcnt[list_idx]++;
	spin_unlock(&nm_i->free_nid_list_lock[list_idx]);
	radix_tree_preload_end();
#else
	spin_lock(&nm_i->free_nid_list_lock);
	if (radix_tree_insert(&nm_i->free_nid_root, i->nid, i)) {
		spin_unlock(&nm_i->free_nid_list_lock);
		radix_tree_preload_end();
		kmem_cache_free(free_nid_slab, i);
		return 0;
	}
	list_add_tail(&i->list, &nm_i->free_nid_list);
	nm_i->fcnt++;
	spin_unlock(&nm_i->free_nid_list_lock);
	radix_tree_preload_end();
#endif
	return 1;
}

static void remove_free_nid(struct f2fs_nm_info *nm_i, nid_t nid) {
	struct free_nid *i;
	bool need_free = false;

#ifdef PER_CORE_NID_LIST
	int list_id = LIST_IDX(nid, nm_i);
	spin_lock(&nm_i->free_nid_list_lock[list_id]);
	i = __lookup_free_nid_list(nm_i, nid);
	if (i && i->state == NID_NEW) {
		__del_from_free_nid_list(nm_i, i);
		nm_i->percore_fcnt[list_id]--;
		need_free = true;
	}
	spin_unlock(&nm_i->free_nid_list_lock[list_id]);
#else
	spin_lock(&nm_i->free_nid_list_lock);
	i = __lookup_free_nid_list(nm_i, nid);
	if (i && i->state == NID_NEW) {
		__del_from_free_nid_list(nm_i, i);
		nm_i->fcnt--;
		need_free = true;
	}
	spin_unlock(&nm_i->free_nid_list_lock);
#endif
	if (need_free)
		kmem_cache_free(free_nid_slab, i);
}

static void scan_nat_page(struct f2fs_sb_info *sbi,
						  struct page *nat_page, nid_t start_nid) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct f2fs_nat_block *nat_blk = page_address(nat_page);
	block_t blk_addr;
	int i;

	i = start_nid % NAT_ENTRY_PER_BLOCK;

	for (; i < NAT_ENTRY_PER_BLOCK; i++, start_nid++) {

		if (unlikely(start_nid >= nm_i->max_nid))
			break;
		blk_addr = le32_to_cpu(nat_blk->entries[i].block_addr);
		f2fs_bug_on(sbi, blk_addr == NEW_ADDR);
		if (blk_addr == NULL_ADDR) {
			if (add_free_nid(sbi, start_nid, true) < 0)
				break;
		}
	}
}

#ifdef PER_CORE_NID_LIST
static void build_all_free_nids(struct f2fs_sb_info *sbi) { // scan all nat pages for free nids
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_HOT_DATA); // only mlog 0 contains NAT journal
	struct f2fs_summary_block *sum = curseg->sum_blk;
	struct f2fs_io_info fio = {
			.sbi = sbi,
			.type = META,
			.rw = READ_SYNC | REQ_META | REQ_PRIO,
			.encrypted_page = NULL,
	};

	block_t start = NAT_BLOCK_OFFSET(nm_i->next_scan_nid);
	nid_t nid = nm_i->next_scan_nid;
	block_t end = NAT_BLOCK_OFFSET(NM_I(sbi)->max_nid);
	int type = META_NAT;
	int i = 0;
	block_t blkno;
	struct page *page;

	for (blkno = start; blkno < end; blkno++) {
		if (!is_valid_blkaddr(sbi, blkno, type))
			goto out;
		fio.blk_addr = current_nat_addr(sbi, blkno * NAT_ENTRY_PER_BLOCK);
		page = grab_cache_page(META_MAPPING(sbi), fio.blk_addr);
		if (!page)
			continue;
		if (PageUptodate(page)) {
			f2fs_put_page(page, 1);
			continue;
		}
		fio.page = page;
		f2fs_submit_page_mbio(&fio);
		f2fs_put_page(page, 0);
	}
	out:
	f2fs_submit_merged_bio(sbi, META, READ);

	while (1) {
		struct page *nat_page = get_current_nat_page(sbi, nid);

		scan_nat_page(sbi, nat_page, nid);
		f2fs_put_page(nat_page, 1);

		nid += (NAT_ENTRY_PER_BLOCK - (nid % NAT_ENTRY_PER_BLOCK));
#ifdef FILE_CELL
		if (unlikely(nid >= nm_i->max_nid))
			nid = NAT_ENTRY_PER_BLOCK;
#else
		if (unlikely(nid >= nm_i->max_nid))
			nid = 0;
#endif
		if (i++ == (blkno - start))
			break;
	}

	mutex_lock(&curseg->curseg_mutex);
	for (i = 0; i < nats_in_cursum(sum); i++) {
		block_t addr = le32_to_cpu(nat_in_journal(sum, i).block_addr);
		nid = le32_to_cpu(nid_in_journal(sum, i));
		if (addr == NULL_ADDR)
			add_free_nid(sbi, nid, true);
		else
			remove_free_nid(nm_i, nid);
	}
	mutex_unlock(&curseg->curseg_mutex);
}

static int build_free_nids(struct f2fs_sb_info *sbi) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_HOT_DATA); // only mlog 0 contains NAT journal
	struct f2fs_summary_block *sum = curseg->sum_blk;
	int i = 0;
	nid_t nid;
	int build = 0;
	/* find free nids from current sum_pages */
	mutex_lock(&curseg->curseg_mutex);
	for (i = 0; i < nats_in_cursum(sum); i++) {
		block_t addr = le32_to_cpu(nat_in_journal(sum, i).block_addr);
		nid = le32_to_cpu(nid_in_journal(sum, i));
		if (addr == NULL_ADDR)
			build += add_free_nid(sbi, nid, true);
		else
			remove_free_nid(nm_i, nid);
	}
	mutex_unlock(&curseg->curseg_mutex);
}

#else
static void build_free_nids(struct f2fs_sb_info *sbi) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_HOT_DATA);
	struct f2fs_summary_block *sum = curseg->sum_blk;
	int i = 0;
	nid_t nid = nm_i->next_scan_nid;
	/* Enough entries */

	if (nm_i->fcnt > NAT_ENTRY_PER_BLOCK) {
		return;
	}
	/* readahead nat pages to be scanned */
	ra_meta_pages(sbi, NAT_BLOCK_OFFSET(nid), FREE_NID_PAGES, META_NAT);

	while (1) {
		struct page *page = get_current_nat_page(sbi, nid);

		scan_nat_page(sbi, page, nid);
		f2fs_put_page(page, 1);

		nid += (NAT_ENTRY_PER_BLOCK - (nid % NAT_ENTRY_PER_BLOCK));
#ifdef FILE_CELL
		if (unlikely(nid >= nm_i->max_nid))
			nid = NAT_ENTRY_PER_BLOCK;
#else
		if (unlikely(nid >= nm_i->max_nid))
			nid = 0;
#endif
		if (i++ == FREE_NID_PAGES)
			break;
	}
	/* go to the next free nat pages to find free nids abundantly */
	nm_i->next_scan_nid = nid;

	/* find free nids from current sum_pages */
	mutex_lock(&curseg->curseg_mutex);
	for (i = 0; i < nats_in_cursum(sum); i++) {
		block_t addr = le32_to_cpu(nat_in_journal(sum, i).block_addr);
		nid = le32_to_cpu(nid_in_journal(sum, i));
		if (addr == NULL_ADDR)
			add_free_nid(sbi, nid, true);
		else
			remove_free_nid(nm_i, nid);
	}
	mutex_unlock(&curseg->curseg_mutex);
}
#endif

/*
 * If this function returns success, caller can obtain a new nid
 * from second parameter of this function.
 * The returned nid could be used ino as well as nid when inode is created.
 */
bool alloc_nid(struct f2fs_sb_info *sbi, nid_t *nid) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct free_nid *i = NULL;
#ifdef PER_CORE_NID_LIST
	retry:
#ifdef PER_CORE_COUNTERS
	if (unlikely(percpu_counter_compare(&sbi->percore_total_valid_node_count, nm_i->available_nids - 1) == 1))
		return false;
#else
	if (unlikely(sbi->total_valid_node_count + 1 > nm_i->available_nids))
		return false;
#endif
	int nid_list_cnt = nm_i->nid_list_count;
	int retry_cnt = 0;
	int list_id = atomic_inc_return(&nm_i->next_allocator) % nid_list_cnt;
	spin_lock(&nm_i->free_nid_list_lock[list_id]);
	/* We should not use stale free nids created by build_free_nids */
	if (nm_i->percore_fcnt[list_id] && !on_build_free_nids(nm_i)) {
		f2fs_bug_on(sbi, list_empty(&nm_i->free_nid_list[list_id]));
		list_for_each_entry(i, &nm_i->free_nid_list[list_id], list) if (i->state == NID_NEW)
				break;

		f2fs_bug_on(sbi, i->state != NID_NEW);
		*nid = i->nid;
		i->state = NID_ALLOC;
		nm_i->percore_fcnt[list_id]--;
		spin_unlock(&nm_i->free_nid_list_lock[list_id]);
		return true;
	}
	spin_unlock(&nm_i->free_nid_list_lock[list_id]);
	build_free_nids:
	/* Let's scan nat pages and its caches to get free nids */
	mutex_lock(&nm_i->build_lock);
	if (nm_i->percore_fcnt[list_id] == 0) {
		build_free_nids(sbi);
	}
	mutex_unlock(&nm_i->build_lock);
	goto retry;
#else
	retry:
#ifdef PER_CORE_COUNTERS
	if (unlikely(percpu_counter_compare(&sbi->percore_total_valid_node_count, nm_i->available_nids - 1) == 1))
		return false;
#else
	if (unlikely(sbi->total_valid_node_count + 1 > nm_i->available_nids))
		return false;
#endif
	spin_lock(&nm_i->free_nid_list_lock);
	/* We should not use stale free nids created by build_free_nids */
	if (nm_i->fcnt && !on_build_free_nids(nm_i)) {
		f2fs_bug_on(sbi, list_empty(&nm_i->free_nid_list));
		list_for_each_entry(i, &nm_i->free_nid_list, list) if (i->state == NID_NEW)
				break;

		f2fs_bug_on(sbi, i->state != NID_NEW);
		*nid = i->nid;
		i->state = NID_ALLOC;
		nm_i->fcnt--;
		spin_unlock(&nm_i->free_nid_list_lock);
		return true;
	}
	spin_unlock(&nm_i->free_nid_list_lock);

	/* Let's scan nat pages and its caches to get free nids */
	mutex_lock(&nm_i->build_lock);
	build_free_nids(sbi);
	mutex_unlock(&nm_i->build_lock);
	goto retry;
#endif
}

/*
 * alloc_nid() should be called prior to this function.
 */
void alloc_nid_done(struct f2fs_sb_info *sbi, nid_t nid) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct free_nid *i;

#ifdef PER_CORE_NID_LIST
	int list_id = LIST_IDX(nid, nm_i);
	spin_lock(&nm_i->free_nid_list_lock[list_id]);
	i = __lookup_free_nid_list(nm_i, nid);
	f2fs_bug_on(sbi, !i || i->state != NID_ALLOC);
	__del_from_free_nid_list(nm_i, i);
	spin_unlock(&nm_i->free_nid_list_lock[list_id]);
	kmem_cache_free(free_nid_slab, i);
#else
	spin_lock(&nm_i->free_nid_list_lock);
	i = __lookup_free_nid_list(nm_i, nid);
	f2fs_bug_on(sbi, !i || i->state != NID_ALLOC);
	__del_from_free_nid_list(nm_i, i);
	spin_unlock(&nm_i->free_nid_list_lock);
	kmem_cache_free(free_nid_slab, i);
#endif
}

/*
 * alloc_nid() should be called prior to this function.
 */
void alloc_nid_failed(struct f2fs_sb_info *sbi, nid_t nid) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct free_nid *i;
	bool need_free = false;

	if (!nid)
		return;

#ifdef PER_CORE_NID_LIST
	int list_id = LIST_IDX(nid, nm_i);
	spin_lock(&nm_i->free_nid_list_lock[list_id]);
	i = __lookup_free_nid_list(nm_i, nid);
	f2fs_bug_on(sbi, !i || i->state != NID_ALLOC);
	if (!available_free_memory(sbi, FREE_NIDS)) {
		__del_from_free_nid_list(nm_i, i);
		need_free = true;
	} else {
		i->state = NID_NEW;
		nm_i->percore_fcnt[list_id]++;
	}
	spin_unlock(&nm_i->free_nid_list_lock[list_id]);
#else
	spin_lock(&nm_i->free_nid_list_lock);
	i = __lookup_free_nid_list(nm_i, nid);
	f2fs_bug_on(sbi, !i || i->state != NID_ALLOC);
	if (!available_free_memory(sbi, FREE_NIDS)) {
		__del_from_free_nid_list(nm_i, i);
		need_free = true;
	} else {
		i->state = NID_NEW;
		nm_i->fcnt++;
	}
	spin_unlock(&nm_i->free_nid_list_lock);
#endif
	if (need_free)
		kmem_cache_free(free_nid_slab, i);
}

void recover_inline_xattr(struct inode *inode, struct page *page) {
	void *src_addr, *dst_addr;
	size_t inline_size;
	struct page *ipage;
	struct f2fs_inode *ri;

	ipage = get_node_page(F2FS_I_SB(inode), inode->i_ino);
	f2fs_bug_on(F2FS_I_SB(inode), IS_ERR(ipage));

	ri = F2FS_INODE(page);
	if (!(ri->i_inline & F2FS_INLINE_XATTR)) {
		clear_inode_flag(F2FS_I(inode), FI_INLINE_XATTR);
		goto update_inode;
	}

	dst_addr = inline_xattr_addr(ipage);
	src_addr = inline_xattr_addr(page);
	inline_size = inline_xattr_size(inode);

	f2fs_wait_on_page_writeback(ipage, NODE);
	memcpy(dst_addr, src_addr, inline_size);
	update_inode:
	update_inode(inode, ipage);
	f2fs_put_page(ipage, 1);
}

void recover_xattr_data(struct inode *inode, struct page *page, block_t blkaddr) {
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	nid_t prev_xnid = F2FS_I(inode)->i_xattr_nid;
	nid_t new_xnid = nid_of_node(page);
	struct node_info ni;

	/* 1: invalidate the previous xattr nid */
	if (!prev_xnid)
		goto recover_xnid;

	/* Deallocate node address */
	get_node_info(sbi, prev_xnid, &ni);
	f2fs_bug_on(sbi, ni.blk_addr == NULL_ADDR);
	invalidate_blocks(sbi, ni.blk_addr);
	dec_valid_node_count(sbi, inode);
	set_node_addr(sbi, &ni, NULL_ADDR, false);

	recover_xnid:
	/* 2: allocate new xattr nid */
	if (unlikely(!inc_valid_node_count(sbi, inode)))
		f2fs_bug_on(sbi, 1);

	remove_free_nid(NM_I(sbi), new_xnid);
	get_node_info(sbi, new_xnid, &ni);
	ni.ino = inode->i_ino;
	set_node_addr(sbi, &ni, NEW_ADDR, false);
	F2FS_I(inode)->i_xattr_nid = new_xnid;

	/* 3: update xattr blkaddr */
	refresh_sit_entry(sbi, NEW_ADDR, blkaddr);
	set_node_addr(sbi, &ni, blkaddr, false);

	update_inode_page(inode);
}

int recover_inode_page(struct f2fs_sb_info *sbi, struct page *page) {
	struct f2fs_inode *src, *dst;
	nid_t ino = ino_of_node(page);
	struct node_info old_ni, new_ni;
	struct page *ipage;

	get_node_info(sbi, ino, &old_ni);

	if (unlikely(old_ni.blk_addr != NULL_ADDR))
		return -EINVAL;
#ifdef FILE_CELL
	ipage = grab_cache_page(NODE_MAPPING(sbi, ino), ino);
#else
	ipage = grab_cache_page(NODE_MAPPING(sbi), ino);
#endif
	if (!ipage)
		return -ENOMEM;

	/* Should not use this inode from free nid list */
	remove_free_nid(NM_I(sbi), ino);

	SetPageUptodate(ipage);
	fill_node_footer(ipage, ino, ino, 0, true);

	src = F2FS_INODE(page);
	dst = F2FS_INODE(ipage);

	memcpy(dst, src, (unsigned long) &src->i_ext - (unsigned long) src);
	dst->i_size = 0;
	dst->i_blocks = cpu_to_le64(1);
	dst->i_links = cpu_to_le32(1);
	dst->i_xattr_nid = 0;
	dst->i_inline = src->i_inline & F2FS_INLINE_XATTR;

	new_ni = old_ni;
	new_ni.ino = ino;

	if (unlikely(!inc_valid_node_count(sbi, NULL)))
		WARN_ON(1);
	set_node_addr(sbi, &new_ni, NEW_ADDR, false);
	inc_valid_inode_count(sbi);
	set_page_dirty(ipage);
	f2fs_put_page(ipage, 1);
	return 0;
}

int restore_node_summary(struct f2fs_sb_info *sbi,
						 unsigned int segno, struct f2fs_summary_block *sum) {
	struct f2fs_node *rn;
	struct f2fs_summary *sum_entry;
	block_t addr;
	int bio_blocks = MAX_BIO_BLOCKS(sbi);
	int i, idx, last_offset, nrpages;

	/* scan the node segment */
	last_offset = sbi->blocks_per_seg;
	addr = START_BLOCK(sbi, segno);
	sum_entry = &sum->entries[0];

	for (i = 0; i < last_offset; i += nrpages, addr += nrpages) {
		nrpages = min(last_offset - i, bio_blocks);

		/* readahead node pages */
		ra_meta_pages(sbi, addr, nrpages, META_POR);

		for (idx = addr; idx < addr + nrpages; idx++) {
			struct page *page = get_meta_page(sbi, idx);

			rn = F2FS_NODE(page);
			sum_entry->nid = rn->footer.nid;
			sum_entry->version = 0;
			sum_entry->ofs_in_node = 0;
			sum_entry++;
			f2fs_put_page(page, 1);
		}

		invalidate_mapping_pages(META_MAPPING(sbi), addr,
								 addr + nrpages);
	}
	return 0;
}

static void remove_nats_in_journal(struct f2fs_sb_info *sbi) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_HOT_DATA); // only mlog 0 contains NAT journal
	struct f2fs_summary_block *sum = curseg->sum_blk;
	int i;

	mutex_lock(&curseg->curseg_mutex);
	for (i = 0; i < nats_in_cursum(sum); i++) {
		struct nat_entry *ne;
		struct f2fs_nat_entry raw_ne;
		nid_t nid = le32_to_cpu(nid_in_journal(sum, i));

		raw_ne = nat_in_journal(sum, i);
#ifdef FILE_CELL
		int tree_idx = TREE_IDX(nid, nm_i);
		down_write(&nm_i->nat_tree_lock[tree_idx]);
		ne = __lookup_nat_cache(nm_i, nid);
		if (!ne) {
			ne = grab_nat_entry(nm_i, nid);
			node_info_from_raw_nat(&ne->ni, &raw_ne);
		}
		__set_nat_cache_dirty(nm_i, ne);
		up_write(&nm_i->nat_tree_lock[tree_idx]);
#else
		down_write(&nm_i->nat_tree_lock);
		ne = __lookup_nat_cache(nm_i, nid);
		if (!ne) {
			ne = grab_nat_entry(nm_i, nid);
			node_info_from_raw_nat(&ne->ni, &raw_ne);
		}
		__set_nat_cache_dirty(nm_i, ne);
		up_write(&nm_i->nat_tree_lock);
#endif
	}
	update_nats_in_cursum(sum, -i);
	mutex_unlock(&curseg->curseg_mutex);
}

static void __adjust_nat_entry_set(struct nat_entry_set *nes,
								   struct list_head *head, int max) {
	struct nat_entry_set *cur;

	if (nes->entry_cnt >= max)
		goto add_out;

	list_for_each_entry(cur, head, set_list) {
		if (cur->entry_cnt >= nes->entry_cnt) {
			list_add(&nes->set_list, cur->set_list.prev);
			return;
		}
	}
	add_out:
	list_add_tail(&nes->set_list, head);
}

static void __flush_nat_entry_set(struct f2fs_sb_info *sbi,
								  struct nat_entry_set *set) {
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_HOT_DATA); // only mlog 0 contains NAT journal
	struct f2fs_summary_block *sum = curseg->sum_blk;
	nid_t start_nid = set->set * NAT_ENTRY_PER_BLOCK;
	bool to_journal = true;
	struct f2fs_nat_block *nat_blk;
	struct nat_entry *ne, *cur;
	struct page *page = NULL;
	struct f2fs_nm_info *nm_i = NM_I(sbi);

#ifdef FILE_CELL
	int tree_idx = -1;
	int tree_idx_tmp = -1;
	int nat_tree_cnt = nm_i->nat_tree_cnt;
#endif

	/*
	 * there are two steps to flush nat entries:
	 * #1, flush nat entries to journal in current hot data summary block.
	 * #2, flush nat entries to nat page.
	 */
	if (!__has_cursum_space(sum, set->entry_cnt, NAT_JOURNAL))
		to_journal = false;

	if (to_journal) {
		mutex_lock(&curseg->curseg_mutex);
	} else {
		page = get_next_nat_page(sbi, start_nid);
		nat_blk = page_address(page);
		f2fs_bug_on(sbi, !nat_blk);
	}

	/* flush dirty nats in nat entry set */
	list_for_each_entry_safe(ne, cur, &set->entry_list, list) {
		struct f2fs_nat_entry *raw_ne;
		nid_t nid = nat_get_nid(ne);
		int offset;
#ifdef FILE_CELL
		tree_idx = TREE_IDX(nid, nm_i);
		f2fs_bug_on(sbi, tree_idx_tmp != -1 && tree_idx != tree_idx_tmp);
		tree_idx_tmp = tree_idx;
#endif
		if (nat_get_blkaddr(ne) == NEW_ADDR)
			continue;

		if (to_journal) {
			offset = lookup_journal_in_cursum(sum, NAT_JOURNAL, nid, 1);
			f2fs_bug_on(sbi, offset < 0);
			raw_ne = &nat_in_journal(sum, offset);
			nid_in_journal(sum, offset) = cpu_to_le32(nid);
		} else {
			raw_ne = &nat_blk->entries[nid - start_nid];
		}
		raw_nat_from_node_info(raw_ne, &ne->ni);
#ifdef FILE_CELL
		down_write(&NM_I(sbi)->nat_tree_lock[tree_idx]);
		nat_reset_flag(ne);
		__clear_nat_cache_dirty(NM_I(sbi), ne);
		up_write(&NM_I(sbi)->nat_tree_lock[tree_idx]);
#else
		down_write(&NM_I(sbi)->nat_tree_lock);
		nat_reset_flag(ne);
		__clear_nat_cache_dirty(NM_I(sbi), ne);
		up_write(&NM_I(sbi)->nat_tree_lock);
#endif
		if (nat_get_blkaddr(ne) == NULL_ADDR)
			add_free_nid(sbi, nid, false);
	}
	if (to_journal)
		mutex_unlock(&curseg->curseg_mutex);
	else
		f2fs_put_page(page, 1);

	f2fs_bug_on(sbi, set->entry_cnt);
#ifdef FILE_CELL
	down_write(&nm_i->nat_tree_lock[tree_idx]);
	radix_tree_delete(&NM_I(sbi)->nat_set_root[tree_idx], set->set);
	up_write(&nm_i->nat_tree_lock[tree_idx]);
#else
	down_write(&nm_i->nat_tree_lock);
	radix_tree_delete(&NM_I(sbi)->nat_set_root, set->set);
	up_write(&nm_i->nat_tree_lock);
#endif
	kmem_cache_free(nat_entry_set_slab, set);
}

/*
 * This function is called during the checkpointing process.
 */
void flush_nat_entries(struct f2fs_sb_info *sbi) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_HOT_DATA); // only mlog 0 contains NAT journal
	struct f2fs_summary_block *sum = curseg->sum_blk;
	struct nat_entry_set *setvec[SETVEC_SIZE];
	struct nat_entry_set *set, *tmp;
	unsigned int found;
	nid_t set_idx = 0;
	LIST_HEAD(sets);
#ifdef FILE_CELL
	int nat_tree_cnt = nm_i->nat_tree_cnt;
	int i;
	unsigned int count1 = 0;
	for (i = 0; i < nat_tree_cnt; i++) {
		down_read(&nm_i->nat_tree_lock[i]);
		count1 += nm_i->percore_dirty_nat_cnt[i];
		up_read(&nm_i->nat_tree_lock[i]);
	}
	nm_i->dirty_nat_cnt = count1;
#endif
	if (!nm_i->dirty_nat_cnt)
		return;
	/*
	 * if there are no enough space in journal to store dirty nat
	 * entries, remove all entries from journal and merge them
	 * into nat entry set.
	 */
	if (!__has_cursum_space(sum, nm_i->dirty_nat_cnt, NAT_JOURNAL))
		remove_nats_in_journal(sbi); // todo
#ifdef FILE_CELL
	for (i = 0; i < nat_tree_cnt; i++) {
		down_write(&nm_i->nat_tree_lock[i]);
		int count = 0;
		set_idx = 0;
		while ((found = __gang_lookup_nat_set(nm_i, i, set_idx, SETVEC_SIZE, setvec))) {
			unsigned idx;
			set_idx = setvec[found - 1]->set + 1;
			for (idx = 0; idx < found; idx++) {
				__adjust_nat_entry_set(setvec[idx], &sets, MAX_NAT_JENTRIES(sum));
			}
			count += found;
		}
		up_write(&nm_i->nat_tree_lock[i]);
	}
#else
	down_write(&nm_i->nat_tree_lock);
	while ((found = __gang_lookup_nat_set(nm_i, set_idx, SETVEC_SIZE, setvec))) {
		unsigned idx;
		set_idx = setvec[found - 1]->set + 1;
		for (idx = 0; idx < found; idx++)
			__adjust_nat_entry_set(setvec[idx], &sets,
								   MAX_NAT_JENTRIES(sum));
	}
	up_write(&nm_i->nat_tree_lock);
#endif
	/* flush dirty nats in nat entry set */
	list_for_each_entry_safe(set, tmp, &sets, set_list) {
		__flush_nat_entry_set(sbi, set);
	}
#ifdef FILE_CELL
#else
	f2fs_bug_on(sbi, nm_i->dirty_nat_cnt);
#endif
}

#ifdef FILE_CELL
static void __flush_nat_entry_set_per_core(struct f2fs_sb_info *sbi,
										   struct per_core_sets_pack *sets) {
	f2fs_bug_on(sbi, !sets);
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_HOT_DATA); // only mlog 0 contains NAT journal
	struct f2fs_summary_block *sum = curseg->sum_blk;
	nid_t start_nid = sets->set_id * NAT_ENTRY_PER_BLOCK;
	bool to_journal = true;
	struct f2fs_nat_block *nat_blk;
	struct nat_entry *ne, *cur;
	struct page *page = NULL;
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	int i;
	const int total_set = sets->next_set_idx;
	int tree_idx = -1;
	int nat_tree_cnt = nm_i->nat_tree_cnt;
	/*
	 * there are two steps to flush nat entries:
	 * #1, flush nat entries to journal in current hot data summary block.
	 * #2, flush nat entries to nat page.
	 */
	if (!__has_cursum_space(sum, sets->entry_cnt, NAT_JOURNAL))
		to_journal = false;

	if (to_journal) {
		mutex_lock(&curseg->curseg_mutex);
	} else {
		page = get_next_nat_page(sbi, start_nid);
		nat_blk = page_address(page);
		f2fs_bug_on(sbi, !nat_blk);
	}

	for (i = 0; i < total_set; i++) {
		/* flush dirty nats in nat entry set */
		list_for_each_entry_safe(ne, cur, &sets->set[i]->entry_list, list) {
			struct f2fs_nat_entry *raw_ne;
			nid_t nid = nat_get_nid(ne);
			tree_idx = nid % nat_tree_cnt;
			f2fs_bug_on(sbi, tree_idx < 0);
			int offset;
			if (nat_get_blkaddr(ne) == NEW_ADDR) {
				continue;
			}

			if (to_journal) {
				offset = lookup_journal_in_cursum(sum, NAT_JOURNAL, nid, 1);
				f2fs_bug_on(sbi, offset < 0);
				raw_ne = &nat_in_journal(sum, offset);
				nid_in_journal(sum, offset) = cpu_to_le32(nid);
			} else {
				raw_ne = &nat_blk->entries[nid - start_nid];
			}
			raw_nat_from_node_info(raw_ne, &ne->ni);
			down_write(&NM_I(sbi)->nat_tree_lock[tree_idx]);
			nat_reset_flag(ne);
			__clear_nat_cache_dirty(NM_I(sbi), ne);
			up_write(&NM_I(sbi)->nat_tree_lock[tree_idx]);

			if (nat_get_blkaddr(ne) == NULL_ADDR)
				add_free_nid(sbi, nid, false);
		}
	}
	if (to_journal)
		mutex_unlock(&curseg->curseg_mutex);
	else
		f2fs_put_page(page, 1);

	for (i = 0; i < nat_tree_cnt; i++) {
		down_write(&nm_i->nat_tree_lock[i]);
		radix_tree_delete(&NM_I(sbi)->nat_set_root[i], sets->set_id);
		up_write(&nm_i->nat_tree_lock[i]);
	}

	for (i = 0; i < total_set; i++) {
		kmem_cache_free(nat_entry_set_slab, sets->set[i]);
	}
	kfree(sets->set);
}


static void __adjust_nat_entry_set_per_core(struct nat_entry_set *nes,
											struct list_head *head, int max, int nat_tree_cnt) {
	struct per_core_sets_pack *cur;
	int flag = 0;
	list_for_each_entry(cur, head, set_list) {
		if (cur->set_id == nes->set) {
			flag = 1;
			break;
		}
	}

	if (flag == 1) {
		cur->set[cur->next_set_idx++] = nes;
		cur->entry_cnt += nes->entry_cnt;
		if (cur->entry_cnt >= max) {
			list_move_tail(&cur->set_list, head);
		}
	} else {
		struct per_core_sets_pack *new_pack;
		new_pack = f2fs_kmem_cache_alloc(per_core_sets_pack_slab, GFP_ATOMIC);
		init_new_per_core_sets_pack(new_pack, nes->set, (unsigned int) nat_tree_cnt);
		new_pack->set[new_pack->next_set_idx++] = nes;
		new_pack->entry_cnt = nes->entry_cnt;
		if (new_pack->entry_cnt >= max || list_empty(head)) {
			list_add_tail(&new_pack->set_list, head);
		} else {
			list_for_each_entry(cur, head, set_list) {
				if (cur->entry_cnt >= new_pack->entry_cnt) {
					list_add(&new_pack->set_list, cur->set_list.prev);
					return;
				}
			}
			list_add_tail(&new_pack->set_list, head);
		}
	}

}

/*
 * This function is called during the checkpointing process.
 */
void flush_nat_entries_per_core(struct f2fs_sb_info *sbi) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_HOT_DATA); // only mlog 0 performs NAT journal
	struct f2fs_summary_block *sum = curseg->sum_blk;
	struct nat_entry_set *setvec[SETVEC_SIZE];
	struct per_core_sets_pack *set, *tmp;
	unsigned int found;
	nid_t set_idx = 0;
	LIST_HEAD(sets);
	int nat_tree_cnt = nm_i->nat_tree_cnt;
	int i;
	unsigned int count1 = 0;
	for (i = 0; i < nat_tree_cnt; i++) {
		down_read(&nm_i->nat_tree_lock[i]);
		count1 += nm_i->percore_dirty_nat_cnt[i];
		up_read(&nm_i->nat_tree_lock[i]);
	}
	nm_i->dirty_nat_cnt = count1;
	if (!nm_i->dirty_nat_cnt)
		return;
	/*
	 * if there are no enough space in journal to store dirty nat
	 * entries, remove all entries from journal and merge them
	 * into nat entry set.
	 */
	if (!__has_cursum_space(sum, nm_i->dirty_nat_cnt, NAT_JOURNAL)) {
		remove_nats_in_journal(sbi); // todo
	}
	int count = 0;
	for (i = 0; i < nat_tree_cnt; i++) {
		down_write(&nm_i->nat_tree_lock[i]);
		set_idx = 0;
		while ((found = __gang_lookup_nat_set(nm_i, i, set_idx, SETVEC_SIZE, setvec))) {
			unsigned idx;
			set_idx = setvec[found - 1]->set + 1;
			for (idx = 0; idx < found; idx++) {
				__adjust_nat_entry_set_per_core(setvec[idx], &sets, MAX_NAT_JENTRIES(sum),
												nat_tree_cnt);
			}
			count += found;
		}
		up_write(&nm_i->nat_tree_lock[i]);
	}
	/* flush dirty nats in nat entry set */
	list_for_each_entry_safe(set, tmp, &sets, set_list) {
		__flush_nat_entry_set_per_core(sbi, set);
		kmem_cache_free(per_core_sets_pack_slab, set);
	}
	for (i = 0; i < nat_tree_cnt; i++) {
		down_read(&nm_i->nat_tree_lock[i]);
		f2fs_bug_on(sbi, nm_i->percore_dirty_nat_cnt[i]);
		up_read(&nm_i->nat_tree_lock[i]);
	}
}

#endif

static int init_node_manager(struct f2fs_sb_info *sbi) {
	struct f2fs_super_block *sb_raw = F2FS_RAW_SUPER(sbi);
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	unsigned char *version_bitmap;
	unsigned int nat_segs, nat_blocks;
	int i;
	nm_i->nat_blkaddr = le32_to_cpu(sb_raw->nat_blkaddr);

	/* segment_count_nat includes pair segment so divide to 2. */
	nat_segs = le32_to_cpu(sb_raw->segment_count_nat) >> 1;
	nat_blocks = nat_segs << le32_to_cpu(sb_raw->log_blocks_per_seg);

	nm_i->max_nid = NAT_ENTRY_PER_BLOCK * nat_blocks;

	/* not used nids: 0, node, meta, (and root counted as valid node) */
	nm_i->available_nids = nm_i->max_nid - F2FS_RESERVED_NODE_NUM;
	nm_i->fcnt = 0;
	nm_i->nat_cnt = 0;
	nm_i->ram_thresh = DEF_RAM_THRESHOLD;
	nm_i->dirty_nat_cnt = 0;

#ifdef PER_CORE_NID_LIST
	int list_cnt = num_online_cpus();
	nm_i->nid_list_count = list_cnt;
	nm_i->nid_chunk = (nm_i->max_nid + 1) / list_cnt + 1;
	nm_i->free_nid_root = kzalloc(list_cnt * sizeof(struct radix_tree_root), GFP_KERNEL);
	nm_i->free_nid_list = kzalloc(list_cnt * sizeof(struct list_head), GFP_KERNEL);
	nm_i->free_nid_list_lock = kzalloc(list_cnt * sizeof(struct spinlock), GFP_KERNEL);
	nm_i->percore_fcnt = kzalloc(list_cnt * sizeof(unsigned int), GFP_KERNEL);

	for (i = 0; i < list_cnt; i++) {
		INIT_RADIX_TREE(&nm_i->free_nid_root[i], GFP_ATOMIC);
		spin_lock_init(&nm_i->free_nid_list_lock[i]);
		INIT_LIST_HEAD(&nm_i->free_nid_list[i]);
		nm_i->percore_fcnt[i] = 0;
	}
#else
	INIT_RADIX_TREE(&nm_i->free_nid_root, GFP_ATOMIC);
	spin_lock_init(&nm_i->free_nid_list_lock);
	INIT_LIST_HEAD(&nm_i->free_nid_list);
#endif

#ifdef FILE_CELL
	if (sbi->nr_file_cell > 0)
		nm_i->nat_tree_cnt = sbi->nr_file_cell;
	else 
		nm_i->nat_tree_cnt = num_online_cpus();
	uint nat_tree_cnt = nm_i->nat_tree_cnt;

	nm_i->nat_root = kzalloc(nat_tree_cnt * sizeof(struct radix_tree_root), GFP_KERNEL);
	nm_i->nat_tree_lock = kzalloc(nat_tree_cnt * sizeof(struct rw_semaphore), GFP_KERNEL);
	nm_i->nat_set_root = kzalloc(nat_tree_cnt * sizeof(struct radix_tree_root), GFP_KERNEL);
	nm_i->nat_entries = kzalloc(nat_tree_cnt * sizeof(struct list_head), GFP_KERNEL);
	nm_i->percore_nat_cnt = kzalloc(nat_tree_cnt * sizeof(unsigned int), GFP_KERNEL);
	nm_i->percore_dirty_nat_cnt = kzalloc(nat_tree_cnt * sizeof(unsigned int), GFP_KERNEL);
	for (i = 0; i < nat_tree_cnt; i++) {
		INIT_RADIX_TREE(&nm_i->nat_root[i], GFP_NOIO);
		init_rwsem(&nm_i->nat_tree_lock[i]);
		INIT_RADIX_TREE(&nm_i->nat_set_root[i], GFP_NOIO);
		INIT_LIST_HEAD(&nm_i->nat_entries[i]);
		nm_i->percore_nat_cnt[i] = 0;
		nm_i->percore_dirty_nat_cnt[i] = 0;
	}
	max_log("list cnt %d, tree cnt :%d\n", list_cnt, nat_tree_cnt);
#else
	INIT_RADIX_TREE(&nm_i->nat_root, GFP_NOIO);
	init_rwsem(&nm_i->nat_tree_lock);
	INIT_RADIX_TREE(&nm_i->nat_set_root, GFP_NOIO);
	INIT_LIST_HEAD(&nm_i->nat_entries);
#endif
	mutex_init(&nm_i->build_lock);
	nm_i->next_scan_nid = le32_to_cpu(sbi->ckpt->next_free_nid);
#ifdef PER_CORE_NID_LIST
	atomic_set(&nm_i->next_allocator, (nm_i->next_scan_nid - 1) % nm_i->nid_list_count);
#endif
	nm_i->bitmap_size = __bitmap_size(sbi, NAT_BITMAP);
	version_bitmap = __bitmap_ptr(sbi, NAT_BITMAP);
	if (!version_bitmap)
		return -EFAULT;

	nm_i->nat_bitmap = kmemdup(version_bitmap, nm_i->bitmap_size,
							   GFP_KERNEL);
	if (!nm_i->nat_bitmap)
		return -ENOMEM;
	return 0;
}

int build_node_manager(struct f2fs_sb_info *sbi) {
	int err;
	sbi->nm_info = kzalloc(sizeof(struct f2fs_nm_info), GFP_KERNEL);
	if (!sbi->nm_info)
		return -ENOMEM;

	err = init_node_manager(sbi);
	if (err)
		return err;
#ifdef PER_CORE_NID_LIST
	build_all_free_nids(sbi);
#else
	build_free_nids(sbi);
#endif
	return 0;
}

void destroy_node_manager(struct f2fs_sb_info *sbi) {
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	struct free_nid *i, *next_i;
	struct nat_entry *natvec[NATVEC_SIZE];
	struct nat_entry_set *setvec[SETVEC_SIZE];
	nid_t nid = 0;
	nid_t set_id = 0;
	unsigned int found;
	int n;
	if (!nm_i)
		return;

	/* destroy free nid list */
#ifdef PER_CORE_NID_LIST
	int nid_list_cnt = nm_i->nid_list_count;
	for (n = 0; n < nid_list_cnt; n++) {
		spin_lock(&nm_i->free_nid_list_lock[n]);
		list_for_each_entry_safe(i, next_i, &nm_i->free_nid_list[n], list) {
			f2fs_bug_on(sbi, i->state == NID_ALLOC);
			__del_from_free_nid_list(nm_i, i);
			nm_i->percore_fcnt[n]--;
			spin_unlock(&nm_i->free_nid_list_lock[n]);
			kmem_cache_free(free_nid_slab, i);
			spin_lock(&nm_i->free_nid_list_lock[n]);
		}
		f2fs_bug_on(sbi, nm_i->percore_fcnt[n]);
		spin_unlock(&nm_i->free_nid_list_lock[n]);
	}
	kfree(nm_i->free_nid_root);
	kfree(nm_i->free_nid_list_lock);
	kfree(nm_i->free_nid_list);
	kfree(nm_i->percore_fcnt);
#else
	spin_lock(&nm_i->free_nid_list_lock);
	list_for_each_entry_safe(i, next_i, &nm_i->free_nid_list, list) {
		f2fs_bug_on(sbi, i->state == NID_ALLOC);
		__del_from_free_nid_list(nm_i, i);
		nm_i->fcnt--;
		spin_unlock(&nm_i->free_nid_list_lock);
		kmem_cache_free(free_nid_slab, i);
		spin_lock(&nm_i->free_nid_list_lock);
	}
	f2fs_bug_on(sbi, nm_i->fcnt);
	spin_unlock(&nm_i->free_nid_list_lock);
#endif
	/* destroy nat cache */
#ifdef FILE_CELL
	int nat_tree_cnt = nm_i->nat_tree_cnt;
	for (n = 0; n < nat_tree_cnt; n++) {
		down_write(&nm_i->nat_tree_lock[n]);
		nid = (nid_t) n;
		while ((found = __gang_lookup_nat_cache(nm_i, n, nid, NATVEC_SIZE, natvec))) {
			unsigned idx;
			nid = nat_get_nid(natvec[found - 1]) + nat_tree_cnt;
			for (idx = 0; idx < found; idx++)
				__del_from_nat_cache(nm_i, natvec[idx]);
		}
		f2fs_bug_on(sbi, nm_i->percore_nat_cnt[n]);

		/* destroy nat set cache */
		set_id = 0;
		while ((found = __gang_lookup_nat_set(nm_i, n, set_id, SETVEC_SIZE, setvec))) {
			unsigned idx;
			set_id = setvec[found - 1]->set + 1;
			for (idx = 0; idx < found; idx++) {
				/* entry_cnt is not zero, when cp_error was occurred */
				f2fs_bug_on(sbi, !list_empty(&setvec[idx]->entry_list));
				radix_tree_delete(&nm_i->nat_set_root[n], setvec[idx]->set);
				kmem_cache_free(nat_entry_set_slab, setvec[idx]);
			}
		}
		up_write(&nm_i->nat_tree_lock[n]);
	}
	kfree(nm_i->nat_root);
	kfree(nm_i->nat_set_root);
	kfree(nm_i->percore_dirty_nat_cnt);
	kfree(nm_i->percore_nat_cnt);
	kfree(nm_i->nat_entries);
	kfree(nm_i->nat_tree_lock);
#else
	down_write(&nm_i->nat_tree_lock);
	while ((found = __gang_lookup_nat_cache(nm_i, nid, NATVEC_SIZE, natvec))) {
		unsigned idx;

		nid = nat_get_nid(natvec[found - 1]) + 1;
		for (idx = 0; idx < found; idx++)
			__del_from_nat_cache(nm_i, natvec[idx]);
	}
	f2fs_bug_on(sbi, nm_i->nat_cnt);

	/* destroy nat set cache */
	nid = 0;
	while ((found = __gang_lookup_nat_set(nm_i, nid, SETVEC_SIZE, setvec))) {
		unsigned idx;

		nid = setvec[found - 1]->set + 1;
		for (idx = 0; idx < found; idx++) {
			/* entry_cnt is not zero, when cp_error was occurred */
			f2fs_bug_on(sbi, !list_empty(&setvec[idx]->entry_list));
			radix_tree_delete(&nm_i->nat_set_root, setvec[idx]->set);
			kmem_cache_free(nat_entry_set_slab, setvec[idx]);
		}
	}
	up_write(&nm_i->nat_tree_lock);
#endif
	kfree(nm_i->nat_bitmap);
	sbi->nm_info = NULL;
	kfree(nm_i);
}

int __init create_node_manager_caches(void) {
	nat_entry_slab = f2fs_kmem_cache_create("nat_entry", sizeof(struct nat_entry));
	if (!nat_entry_slab)
		goto fail;

	free_nid_slab = f2fs_kmem_cache_create("free_nid", sizeof(struct free_nid));
	if (!free_nid_slab)
		goto destroy_nat_entry;

	nat_entry_set_slab = f2fs_kmem_cache_create("nat_entry_set", sizeof(struct nat_entry_set));

	if (!nat_entry_set_slab)
		goto destroy_free_nid;
#ifdef FILE_CELL
	per_core_sets_pack_slab = f2fs_kmem_cache_create("per_core_sets_pack",
													 sizeof(struct per_core_sets_pack));
	if (!per_core_sets_pack_slab) {
		goto destroy_per_core_sets_pack;
	}
#endif
	return 0;

	destroy_free_nid:
	kmem_cache_destroy(free_nid_slab);
	destroy_nat_entry:
	kmem_cache_destroy(nat_entry_slab);
#ifdef FILE_CELL
	destroy_per_core_sets_pack:
	kmem_cache_destroy(per_core_sets_pack_slab);
#endif
	fail:
	return -ENOMEM;
}

void destroy_node_manager_caches(void) {
	kmem_cache_destroy(nat_entry_set_slab);
	kmem_cache_destroy(free_nid_slab);
	kmem_cache_destroy(nat_entry_slab);
#ifdef FILE_CELL
	kmem_cache_destroy(per_core_sets_pack_slab);
#endif
}
