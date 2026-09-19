/* NovaOS - built-in graphical desktop (mode 13h, 320x200x256)
 * This is the first iteration of the NovaOS own graphics stack:
 * framebuffer painting + a tiny window manager + taskbar.
 * Keyboard: Tab = cycle focus, Esc = close window, C = open console
 * window, A = open about window.
 */
#ifndef NOVA_GUI_H
#define NOVA_GUI_H

#include "common.h"

void gui_enter(void);   /* switches to mode 13h and runs the desktop loop */

#endif
