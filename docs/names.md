# Library & Program names

Some libraries, systems and programs in \[REDACTED\] OS have non-conventional names. This is a list of them:

## Libraries

- uno: ui library. There's two versions of it, an immediate mode version found in /shared/uno and a retained mode version provided as a standalone. The standalone version is more feature complete and is meant to replace the immediate mode version.
- dos: desktop. Used to create win*do\[w\]s* in the system. Part /kernel/kernel_processes
- tres: compositing & windowing library. It might get merged with dos soon, it also should've probably been the one named dos.
- cuatro: Audio library. Very basic audio output

## Directories

- /console: system console output. Similar to /dev/console
- /clipboard: system's main way of copy-pasting by using the filesystem
- /language: a (stub) LFSP (Language File System Protocol), an LSP-like tool. This should live in userspace but due to the filesystem being kernelspace only, it's part of the kernel for now.
- /theme: reflects visual preferences on the OSs look, like desktop background color and contrasting foreground color.
- /tools: similar to /bin, command line tools

NOTE: due to processes having virtualized/isolated filesystem namespaces, and this system not being UNIX-like, some directories and files with the same name as their UNIX counterpart may not work entirely as expected. As an example, /random is not (yet) cryptographically secure, /home is the ONLY home folder, since the system is single-user, like how /home/<user> works on Linux.

## Programs

- tools: The system's version of /bin. Once the virtual filesystem becomes more powerful, binding one or more folders to /tools will make their contents available to the terminal, similar to how PATH works on UNIX-likes.
- read: reading the contents of a file. Similar to the most common use case for cat on UNIX, but not meant to concatenate files.
- manual: system docs, similar to man

Aliases will be introduced to the terminal at some point, and programs like read and manual may be aliased to their shorter but less self-documenting cat/man versions, even if they won't work the same way they do on UNIX-likes.