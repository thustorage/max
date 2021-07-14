#ifdef _MTRACE
#include <mtrace-magic.h>
#else
#define mtrace_enable_set(x, y, z) do { } while (0)
#endif
