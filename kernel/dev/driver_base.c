#include "driver_base.h"

uint64_t fd_id = 0;

uint64_t reserve_fd_id(){
    return ++fd_id;
}