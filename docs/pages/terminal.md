# Terminal

## Overview

The system's terminal has the same basic functionality as any other in terms of text input, history, etc, but differs greatly in its implementation.
Its default shell (`sheldon`) provides some commands such as `echo` or `ls` as built-in, which simply wrap around `redlib` functions. These built-ins do not run in separate processes. Anything not registered as builtin will be run from `/tools` inside the system, which maps to `/fs/redos/tools` inside the project's root. 
It can be configured through some special files inside `/environment` to render structured data

## developers

When the terminal launches a non-built-in program from `/tools`, it launches as its own process. The terminal will keep track of its `proc_id` and poll `/proc/<id>/out` and `/proc/<id>/state` to print its output to the screen and check when the program has exited. 
`/proc/<id>/out` is automatically written to by `redlib`'s `print` and `put` functions (they work like libc's `printf` but print adds a newline at the end, whereas put does not).
Each process' output is automatically assigned file descriptor 2 (see file descriptors in the filesystem section).

Prints can be formatted with a simple markup language. While under development, the language allows for setting a text color, background color, moving the cursor and wiping the screen. Some examples:
* `{color:ff0000 this will be red} this will not`.
* `{wipe}`
* `{move:10,10} Cursor begins at 10,10`
`{` begins an instruction, `}` ends it, `:` separates an instruction from an argument, `,` separates arguments, ` ` separates the command from the contents.
Nesting these instructions will be supported in the future but will lose any formatting as soon as the innermost format instruction ends 

### Structured data

The system also has WIP support for rendering structured data (such as tables and single entries). It relies on redlib's binary serializer `data/serialize/binary_serial` and on several special files inside `/environment`, a helper file `environment/env_types.h` is provided. 
1. Inform the terminal of your program's display requirements with `set_environment_config((env_config){<env_display_type>,<env_behavior>});` or write the struct directly to `/environment/config`. Only the document (single entry) display type is currently supported. Behavior currently only informs the terminal if it should wipe the screen or scroll when the data is updated.
2. Inform the system of the new config by sending `\[` through regular process output (`print` or `put`)
3. Define the data structure with a structdef array, such as: `{{ .type = binary_type_i32,      .name = string_from_literal("number") }, ... }`. Inform the system by sending `\1` through regular output.
5. Serialize the format with `set_environment_structure` (in the helper, or send it to `/environment/structure` after calling the serializer's `bin_ser_emit_structure` on it) passing the array and number of entries.
6. Create a binary serializer using the data structure you defined `make_binary_serializer` and serialize data with `bin_ser_serialize`, passing the serializer, data, data_size and count. Note that only packed structs are supported for the time being.
7. Send the serialized data with `send_environment_data` or writing it to `/environment/data`. Inform the system with `\3`.

The terminal can also send input, currently limited to ASCII commands to the currently running process. `CTRL+C` will send a `QUIT` signal (the process could choose ignore it), `CTRL+Z` to send a `STOP` signal and halt execution, and `CTRL+V` to resume with a `CONT` signal.

As mentioned in the UI section, terminal support only works until a process requests a draw context, at which point the terminal will fully disconnect from it.