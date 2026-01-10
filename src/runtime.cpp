#include "plexus/runtime.h"
#include "thread_pool.h"

namespace Plexus {

    size_t current_worker_index() { return t_worker_index; }

    size_t worker_count() { return t_worker_count; }

} // namespace Plexus
