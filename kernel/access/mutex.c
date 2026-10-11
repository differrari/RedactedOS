#include "mutex.h"
#include "exceptions/irq.h"
#define MOG_MODULE "mutex"
#define MOG_LEVEL MOG_VERBOSE
#include "utils/mog.h"

hash_map_t *mutexed_resources;

u32 mutex_owner_id = 1;

mutex_owner new_mutex_owner(){
    return mutex_owner_id++;
}

bool lock_mutex(mutex_owner owner, resource r){
    irq_flags_t irq = irq_save_disable();
    if (!mutexed_resources) mutexed_resources = hash_map_create(256);
    mutex_owner locker = ((u64)hash_map_get(mutexed_resources, &r, sizeof(resource)) & UINT32_MAX);
    if (locker) return false;
    hash_map_put(mutexed_resources, &r, sizeof(resource), (void*)(u64)owner);
    locker = ((u64)hash_map_get(mutexed_resources, &r, sizeof(resource)) & UINT32_MAX);
    bool res = locker == owner;
    irq_restore(irq);
    return res;
}

bool unlock_mutex(mutex_owner owner, resource r){
    if (!mutexed_resources) return false;
    mutex_owner mutexer = ((u64)hash_map_get(mutexed_resources, &r, sizeof(resource)) & UINT32_MAX);
    if (owner != mutexer){
        mog(ERROR,"mutex owner %i does not own resource %llx",owner,r.resource);
        return false;
    }
    hash_map_remove(mutexed_resources, &r, sizeof(resource), 0);
    return true;
}
