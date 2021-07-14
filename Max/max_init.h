
#include <linux/mutex.h>

#include "rps.h"

struct max_info {
	struct rps rps_cp_rwsem;
	struct rps rps_node_write;
};