#include <hardware/clocks.h>
#include <hardware/watchdog.h>
#include <pico/multicore.h>
#include <pico/stdlib.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void setup_video();
extern void setup_audio();

extern void step_video();

int main() {
    stdio_init_all();

    printf("waiting for serial");
    
    sleep_ms(2000);
    printf(".");
    
    set_sys_clock_khz(288000, true);

    clock_configure(
        clk_hstx,
        CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
        CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
        288000,
        125000
    );

    setup_hstx();
    busy_wait_ms(2000);

    multicore_launch_core1(setup_audio);
    setup_video();

    uint32_t t;
    while (true) {
        t = time_us_32();

        // stream video
        step_video();

        // wait until correct framerate
        uint32_t delta = (time_us_32() - t);
        busy_wait_us_32((1000000 / 30) - delta);
    }
}
