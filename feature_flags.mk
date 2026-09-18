# Add extra compilation flags meant to enable or disable features at compile time. 
# For runtime create a system.config inside ./fs/redos/system.config

# CFLAGS_BASE += -DBUG_CRASH_FSUS # disable or enable a sometimes crashing launcher functionality using the package manager
# CFLAGS_BASE += -DBUG_RESET_TTBR0 # disable or enable a crashing cleanup function in virtual memory
CFLAGS_BASE += -DFEATURE_MENU # disable or enable the menubar
# CFLAGS_BASE += -DMODULE_STRICT # disable or enable strict checking for modules. May panic unnecessarily