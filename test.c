/* test.c — PopLine 完整测试套件 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "popline.h"

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}
