#include "config.h"
#ifdef USE_M5STACK_CORE_BOOT
#include <M5Stack.h>

int specific_boot() {
    M5.begin();
    return 1;
}

#endif
