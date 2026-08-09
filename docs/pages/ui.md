# UI

![The system has a window-based UI system, which is integrated into the kernel itself](overview). It's made up of two parts,  ![dos (/kernel/kernel_processes/dos) & tres (/kernel/graph/tres). Dos is the desktop environment, tres handles creating & compositing the actual windows](overview). Due to the similarities between the two, they might be merged in the future, or their names swapped/changed.

## Windowing & Desktop

![Windows are drawable by dragging and dropping on the desktop](overview/windows/desktop). 
![Windows don't overlap, so windows will move themselves to the closest available space.](overview/windows/desktop)
![There's currently no window controls (close, minimize, maximize) or visible dock. Visual feedback is limited](overview/desktop)
![By default a new window opens on an application launcher. It takes applications from `/home/applications` (which is a shared folder, explained in the filesystem section) and `/boot/redos/system` (which maps to `/fs/redos/system`)](overview/windows)
![The desktop is infinite. Drag with middle mouse to move around it.](overview/desktop) Zooming exists but is currently disabled and requires patching the kernel to enable. It might be added as a setting eventually.

The desktop has a "drawing mode", which just draws lines on the space between windows.
Pressing ![META/CMD + 1 will move the desktop to windowing mode, META/CMD + 2 will put it in drawing mode](overview/desktop)

The desktop can have an image background, ![place a `desktop.png` image inside `fs/redos` to be loaded as a background image to the desktop](overview/desktop). It will automatically set the system tint (which would've been the desktop background if no image were loaded) to the average color of the image, and the foreground color to a complementary.

## Developers

The `draw_ctx` type (`shared/graphic_types.h`) is used to hold window/framebuffer information. 
Call `request_draw_ctx(draw_ctx*)`, providing an empty `draw_ctx` object for the system to provide your application with a framebuffer (`draw_ctx.fb`), as well as width/height/stride information `draw_ctx.width/height/stride`.
* Pre-filling the `width` and `height` of the `draw_ctx` before calling `request_draw_ctx` will resize the window to the provided size.
* If an application is opened from the terminal, its default behavior is to use no graphics, receive no input and print its console output to the terminal's screen. Calling `request_draw_ctx` will overwrite this behavior and make the program completely standalone, receiving input and drawing to the screen directly.
Inform the system to refresh the window with `commit_draw_ctx(draw_ctx*)`. Inside the draw_ctx, inform the system of which ranges to re-render (`draw_ctx.dirty_rects/dirty_count`) or ask it to re-render the entire window (`draw_ctx.full_redraw`).
If needed, the window can be resized with `resize_window_ctx(draw_ctx*)`. The window will resize and move to an empty area to not overlap with any windows if needed, and the draw_ctx will be updated with the new size and a new framebuffer.
All colors are 32-bit, as indicated by the `color` and `argbcolor` redlib types (`shared/graphic_types.h`). The colors are stored internally as BBGGRRAA, but due to endianness, they can be written as 0xAARRGGBB in hexadecimal. `argbcolor` is a union divided by channel.