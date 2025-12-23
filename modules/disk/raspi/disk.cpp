#include "filesystem/disk.h"
#include "sdhci.hpp"
#include "hw/hw.h"
#include "console/kio.h"

SDHCI sdhci_driver; 

extern "C" void disk_verbose(){
    sdhci_driver.enable_verbose();
}

extern "C" bool init_disk_device(){
    kprint("Initializing disk");
    return sdhci_driver.init();
}

extern "C" void disk_write(const void *buffer, uint32_t sector, uint32_t count){
    sdhci_driver.write(buffer, sector, count);
}

extern "C" void disk_read(void *buffer, uint32_t sector, uint32_t count){
    sdhci_driver.read(buffer, sector, count);
}

system_module disk_module = (system_module){
    .name = "sdhci",
    .mount = "/disk",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = init_disk_device,
    .fini = 0,
    .open = 0,
    .read = 0,
    .write = 0,
    .close = 0,
    .sread = 0,
    .swrite = 0,
    .readdir = 0,
};