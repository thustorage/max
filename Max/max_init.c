#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include "max_fs.h"
#include "rps.h"
#include "f2fs.h"

int init_max_info(struct f2fs_sb_info *sbi) {
	sbi->max_info = kmalloc(sizeof(struct max_info), GFP_KERNEL);
	struct max_info *max_i;
	max_i = sbi->max_info;
	if (!max_i) {
		// error
		return -ENOMEM;
	}
#ifdef RPS
	rps_init_rwsem(&max_i->rps_cp_rwsem);
	rps_init_rwsem(&max_i->rps_node_write);
#endif

#ifdef FILE_CELL
	if(sbi->nr_file_cell > 0) 
		sbi->node_count = sbi->nr_file_cell;
	else 
		sbi->node_count = num_online_cpus();
	
	if (sbi->node_count > NAT_ENTRY_PER_BLOCK - 3) {
		f2fs_msg(sbi->sb, KERN_ERR, "Max does support so many file cells");
		return -1;
	}
#endif

#ifdef MLOG
	atomic_set(&sbi->next_mlog, 0);
#endif

	return 1;
}

int destroy_max_info(struct f2fs_sb_info *sbi) {
	struct max_info *max_info = sbi->max_info;
	if (!max_info)
		return 1;
#ifdef RPS
	rps_free_rwsem(&max_info->rps_cp_rwsem);
	rps_free_rwsem(&max_info->rps_node_write);
#endif
	kfree(max_info);
	sbi->max_info = NULL;
	return 1;
}