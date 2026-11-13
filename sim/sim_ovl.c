/*
 * sim_ovl.c -- overlays without a window: every overlay's code is linked in, so ovl_get() hands
 * back its entry-point table straight from the header startup/ap_ovl.c defines. The "modes'
 * window" is still tracked (ovl_current) so the UI sees the same mode changes as on the device.
 */
#include <stddef.h>

#include "sim.h"
#include "services/ovl.h"

extern const ovl_hdr_t ovl_hdr_usbdac, ovl_hdr_usbout, ovl_hdr_dev, ovl_hdr_fsw, ovl_hdr_lib,
    ovl_hdr_libtag, ovl_hdr_lwalk, ovl_hdr_lbuild;

static const ovl_hdr_t *const k_hdr[OVL_COUNT] = {
    [OVL_USBDAC] = &ovl_hdr_usbdac, [OVL_USBOUT] = &ovl_hdr_usbout, [OVL_DEV] = &ovl_hdr_dev,
    [OVL_FSW] = &ovl_hdr_fsw,       [OVL_LIB] = &ovl_hdr_lib,       [OVL_LIBTAG] = &ovl_hdr_libtag,
    [OVL_LWALK] = &ovl_hdr_lwalk,   [OVL_LBUILD] = &ovl_hdr_lbuild,
};

static unsigned s_cur;                 /* the modes' window (win 0) */
static int s_hold;

const void *ovl_get(unsigned id)
{
    if (id == OVL_NONE || id >= OVL_COUNT)
        return NULL;
    if (ovl_desc[id].win == 0 && s_cur != id) {
        if (s_hold)
            return NULL;               /* code in the window still runs */
        if (s_cur && ovl_desc[s_cur].stop)
            ovl_desc[s_cur].stop();
        s_cur = id;
    }
    return k_hdr[id]->api;
}

const void *ovl_if(unsigned id)
{
    if (id == OVL_NONE || id >= OVL_COUNT)
        return NULL;
    if (ovl_desc[id].win == 0 && s_cur != id)
        return NULL;
    return k_hdr[id]->api;
}

void ovl_run(unsigned id, unsigned n)
{
    void (*const *api)(void) = ovl_get(id);

    if (!api)
        return;
    ovl_hold(1);
    api[n]();
    ovl_hold(-1);
}

unsigned ovl_current(void) { return s_cur; }
const char *ovl_why(unsigned id) { return NULL; }
void ovl_hold(int d) { s_hold += d; }
