/*
 * sim_protos.h -- force-included into every simulator object (-include, sim/Makefile).
 *
 * A few firmware files call a function without its declaration in scope. On the device that is
 * harmless (int and pointers are both 32-bit); on a 64-bit PC an undeclared function returning a
 * pointer has the pointer cut to 32 bits. These are the declarations those files miss, exactly
 * as their headers declare them.
 */
#ifndef RECHORD_SIM_PROTOS_H
#define RECHORD_SIM_PROTOS_H
#include <stdint.h>
#include <stdio.h>                    /* snprintf (player/player_audio.c) */

const char *panel_io_path(void);      /* drivers/panel_io.h  (ui/screens.c) */
uint32_t time_ms(void);               /* drivers/clock.h     (ui/frame.c) */
int lib_add_step(uint32_t us);        /* ui/lib_ui.c         (ui/lib_ui.c, before its definition) */

#endif
