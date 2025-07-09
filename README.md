# Redacted Operating System

## Summary

The REDACTED Operating System is a 64 bit operating system meant to run on ARM processors. 
It currently runs entirely in QEMU, either in an entirely virtual machine using QEMU's `virt` board, or emulating a Raspberry Pi 4B. It currently most likely will not work on real hardware (not on the raspi 5 anyway, haven't tested on a 4B) 
Its GUI is heavily inspired by video game consoles, and the OS is entirely controllable via keyboard (with future mouse and possibly gamepad support)
It has limited support for third party processes, currently limited to entirely self-contained programs. The `shared` library is provided for syscalls and common functions but it should be used as a static library embedded into the final executable `.elf` file.

The OS is a hobby project, and my first attempt at writing an OS, so it's subject to improvement, and most likely isn't the most reliable and certainly not the cleanest code.

## Project organization

The project is divided into 3 main sections, each represented by a folder in its main directory.

### Kernel

Contains the kernel code, this code is the entry point to the system and responsible for its initialization.
The system's entry point is the `boot.s` file (see aboutboot.txt in the kernel folder if there's issues with the boot file). The boot file loads a stack pointer defined in the `linker.ld` file, which also defines where the kernel is loaded. It then jumps to the `kernel_main` function located in `kernel.c`.
The actual system initialization happens in `kernel.c`'s `kernel_main` function, which is responsible for initializing the system until the point where it's handed off to processes.

Once the system is initialized, most of the system's functionality is provided by processes, which includes kernel processes and user processes. User processes are defined in their own section of the readme file, but kernel processes are included directly into the kernel. While they're placed in their own section defined in the linker, there's no real distinction between their code and any other kernel code. 
Certain parts of the system initialization, such as GPU initialization and XHCI input code initialization rely on some syscalls that expect a process to be running, so an initial kernel.c process is created to hold their data. In the future they'll each have their own process.
Everything about kernel processes and the pseudo-processes mentioned for GPU/XHCI will most likely be improved in the future, to make the system more modular, and kernel processes might get loaded from filesystem, in a similar way to how user processes are loaded. In order to do this, they either need to not rely on any kernel-only code (as several of them currently do) and rely on syscalls entirely, or (less ideal) dynamically link to the kernel. Doing this will increase the kernel's modularity.

The system can run from both the `kernel.elf` and `kernel.img` files, on both virt and raspi4b.

### Shared

A shared library made available to both the kernel and external user processes.
It provides very limited implementations for a standard library (std). It includes definitions for the C++ new and delete operators, basic implementations for fixed-size arrays and index maps, which will most likely be expanded into linked lists in order to remove the fixed-size limitation. The library also includes string and memory copy/clear/set functions. These can be accessed by including the `std/std.h` header.
It provides a UI library, which includes simple 2D drawing functions to write pixels directly through a framebuffer. This is not needed by processes at this time, as syscalls that draw onto the screen directly will be provided, but this will be changed in the future. It also includes a basic C++ Label class, and will be expanded to have more common UI types in the future. These can be accessed by including the `ui/ui.h` header.
The shared library also contains simple math functions. These can be accessed by including the `math/math.h` header.
Finally, it includes a header that defines a structure for handling keyboard input from the system, as well as basic keycodes.

### User

This section contains the code for user processes. The code is currently compiled into single a single executable, which comes in a .elf and .bin format. Currently, only .elf format is supported by the system.
Due to the lack of dynamic linking in the system, the code must be entirely self-contained, and any external library it uses must be included in the final binary file. The system's shared library, which provides useful type definitions and syscalls is compiled as a .a static library file which can be linked into the final binary file as `shared/libshared.a`.

The compiled binaries (both the .elf and .bin) are automatically moved into the fs/ folder, from which a system-readable image is created to make it accessible to the system. 

This is (theoretically) not the only way to run user processes on the system. Placing a .elf file inside the fs/redos/user/ folder and running the createfs command (or an equivalent if running on a non-macOS host system) should make them readable and executable by the system, though a limit of 9 processes exists due to array and desktop ui limitations that will be lifted in the future as the system evolves.

This folder will most likely be removed in the future, to be replaced by standalone projects that create user processes.

### Support files

In addition to the code located in the system's 3 main folders, a few other files in the root directory of the project provide useful commands for compiling and running REDACTED OS.

- run_virt: runs REDACTED OS in QEMU using a compatible virt board and devices. It's possible to attach gdb to it by passing the debug argument and additionally running the `debug` command, a shortcut for this is the `rundebug` command.
- run_raspi: runs REDACTED OS in QEMU using a compatible raspberry pi 4b board. It's possible to attach gdb to it by passing the debug argument and additionally running the `debug` command, a shortcut for this is the `rundebug` command. This version is quite slow and may not have all the features found in a real raspberry pi or on the virt board.
- debug: attaches gdb to an instance of QEMU running REDACTED OS. The instance must be run with the `debug` argument as `./run debug` in order for gdb to attach and control its execution. It can support automatically running a command inside gdb such as adding a breakpoint by passing it as an argument as `./debug b kernel_main`
- rundebug: a shortcut for running both the `run` and `debug` commands. It can pass arguments for the debug command, which in turn will pass them to gdb
- createfs: creates a filesystem image for the system to read. The image does not contain kernel code and is not used to boot the system, but is still required to boot the system, and contains user processes. The script is written specifically for macOS, as it relies on the diskutil commands to create the image. There isn't a linux or windows version for it, but a filesystem image can be created manually and placed in the root directory with the name disk.img
- Makefile: can compile and run the system. `make run` compiles the system for the virt board, passing MODE=virt/raspi specifies which board to compile for. It creates the filesystem image and executes the correct `./run` command. `make debug` compiles the system, creates the filesystem and executes the OS and gdb, through the `./rundebug` command. `make all` simply compiles and creates the filesystem image. `make clean` cleans compiled files.

## Compiling

In order to compile the system, you'll need to have the ARM GNU toolchain, for which ARM should provide binaries that run on your host system.
The OS currently only runs on QEMU, so QEMU needs to be installed as well. The system can run entirely on QEMU devices which are included in the pre-built QEMU binaries, but could optionally also support other devices that are not available by default. To use them, QEMU must be compiled by source and configured to include those devices. 
Running `make run` or `make all` followed by `./run` is the fastest way to run the system, using the default configuration (virt board for QEMU). Further explanations on these commands and their options provided in the Supporting Files section.
Running `make run MODE=virt/raspi` can select which board to compile the code for.
In order to run the OS, you'll need to create a folder called fs inside the project's root directory. This folder will contain user processes. It must have the following subfolders: /redos/user/, with user processes placed inside with the .elf extension. The `user` process will automatically be placed in that folder when compiling. A script called `createfs` will run automatically to create an image from the fs folder. This script currently only works on MacOS and Linux host systems, it will need to be adapted to run on Windows systems. Since my main host system is Mac, there's a chance the linux version won't be up to date if any changes need to be made.

Github Actions should automatically compile changes made to the `main` branch using Mac, so a compiled version of the system with all the files needed to run it should be found there. This includes the fs folder, containing the filesystem, but you'll need to recompile the `disk.img` filesystem with `createfs` for any changes made to this folder to be reflected. Currently it only builds a version for the virt board.

## Raspberry Pi Compatibility

The project currently has preliminary compatibility with raspberry pi. Currently only tested on QEMU. It can run on the 4b board, and theoretically has support for the 3, but has not been tested on it. Earlier boards will not work due to only supporting 32 bit systems.
The Raspberry Pi emulation has the following limitations:
- It does not have networking capabilities
- It does not use interrupts to detect keystrokes and process them, and relies on polling, which can be quite slow.
- It does not use DMA for disk (SD Card) transfers, and may freeze for several seconds while loading data

## Networking

The system has basic networking support. Currently, it performs DHCP discovery and request to receive an IP address on the local network, is capable of responding to Ping and ARP, and can connect to a server running on ports 8080 and 80, though it currently does nothing noteworthy.
An implementation of the server can be found at the [RedactedOS Firmware Server Repository](https://github.com/differrari/RedactedOS_firmware_server/tree/main)

