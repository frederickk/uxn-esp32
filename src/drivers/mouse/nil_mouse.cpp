#include "config.h"
#ifdef USE_NIL_MOUSE
extern "C" {
#include <uxn.h>
}

int
devmouse_init()
{
    return 1;
}

int
devmouse_handle(Device *d)
{
    (void)d;
    return 1;
}

#endif
