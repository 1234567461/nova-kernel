/* NovaOS - PS/2 mouse driver (auxiliary device, IRQ12) */
#ifndef NOVA_MOUSE_H
#define NOVA_MOUSE_H

#include "common.h"

void mouse_init(void);
/* tell the driver the real screen size and the motion scale to use */
void mouse_set_bounds(int w, int h, int scale);
int  mouse_pos_x(void);
int  mouse_pos_y(void);
int  mouse_left_down(void);
int  mouse_left_clicked(void);   /* edge: 0 -> 1, consumes the edge */
int  mouse_left_released(void);  /* edge: 1 -> 0, consumes the edge */

#endif
