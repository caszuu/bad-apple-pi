#include <pico/multicore.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static uint8_t *active_fb = 0x20000000;
static uint8_t dvi_fb[640 * 180];

/* dvi driver */

#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <hardware/structs/bus_ctrl.h>
#include <hardware/structs/hstx_ctrl.h>
#include <hardware/structs/hstx_fifo.h>

// ----------------------------------------------------------------------------
// DVI constants

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define MODE_H_SYNC_POLARITY 0
#define MODE_H_FRONT_PORCH   16
#define MODE_H_SYNC_WIDTH    96
#define MODE_H_BACK_PORCH    48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_SYNC_POLARITY 0
#define MODE_V_FRONT_PORCH   10
#define MODE_V_SYNC_WIDTH    2
#define MODE_V_BACK_PORCH    33
#define MODE_V_ACTIVE_LINES  480

/* #define MODE_H_SYNC_POLARITY 0
#define MODE_H_FRONT_PORCH   8
#define MODE_H_SYNC_WIDTH    16
#define MODE_H_BACK_PORCH    24
#define MODE_H_ACTIVE_PIXELS 240

#define MODE_V_SYNC_POLARITY 1
#define MODE_V_FRONT_PORCH   3
#define MODE_V_SYNC_WIDTH    4
#define MODE_V_BACK_PORCH    6
#define MODE_V_ACTIVE_LINES  180 */

#define MODE_H_TOTAL_PIXELS ( \
    MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + \
    MODE_H_BACK_PORCH  + MODE_H_ACTIVE_PIXELS \
)
#define MODE_V_TOTAL_LINES  ( \
    MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + \
    MODE_V_BACK_PORCH  + MODE_V_ACTIVE_LINES \
)

#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// ----------------------------------------------------------------------------
// HSTX command lists

// Lists are padded with NOPs to be >= HSTX FIFO size, to avoid DMA rapidly
// pingponging and tripping up the IRQs.

static uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V1_H1,
    HSTX_CMD_NOP
};

static uint32_t vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V0_H1,
    HSTX_CMD_NOP
};

static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS       | MODE_H_ACTIVE_PIXELS
};

// ----------------------------------------------------------------------------
// DMA logic

// #define DMACH_PING 0
// #define DMACH_PONG 1

#define DMACH_NUM 4
#define DVI_DMA_IRQ 0

// First we ping. Then we pong. Then... we ping again.
static uint32_t ch_num = 0;

// A ping and a pong are cued up initially, so the first time we enter this
// handler it is to cue up the second ping after the first ping has completed.
// This is the third scanline overall (-> =2 because zero-based).
static uint v_scanline = DMACH_NUM;

// During the vertical active period, we take two IRQs per scanline: one to
// post the command list, and another to post the pixels.
static bool vactive_cmdlist_posted = false;

void __time_critical_func(dma_irq_handler)() {
    // dma_pong indicates the channel that just finished, which is the one
    // we're about to reload.
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;

    ch_num = (ch_num + 1) % DMACH_NUM;

    if (v_scanline >= MODE_V_FRONT_PORCH && v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH)) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
    } else if (v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
    } else if (!vactive_cmdlist_posted) {
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
        vactive_cmdlist_posted = true;
    } else {
        uint32_t prescale_scanline = (v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES)) * 180 / MODE_V_ACTIVE_LINES;

        ch->read_addr = (uintptr_t)&dvi_fb[prescale_scanline * MODE_H_ACTIVE_PIXELS];
        ch->transfer_count = MODE_H_ACTIVE_PIXELS / sizeof(uint32_t);
        vactive_cmdlist_posted = false;
    }

    if (!vactive_cmdlist_posted) {
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
    }
}

void setup_hstx() {
    // Configure HSTX's TMDS encoder for RGB332
    hstx_ctrl_hw->expand_tmds =
        2  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        2  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        1  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    // Pixels (TMDS) come in 4 8-bit chunks. Control symbols (RAW) are an
    // entire 32-bit word.
    hstx_ctrl_hw->expand_shift =
        4 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        8 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    // Serial output config: clock period of 5 cycles, pop from command
    // expander every 5 cycles, shift the output shiftreg by 2 every cycle.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // Note we are leaving the HSTX clock at the SDK default of 125 MHz; since
    // we shift out two bits per HSTX clock cycle, this gives us an output of
    // 250 Mbps, which is very close to the bit clock for 480p 60Hz (252 MHz).
    // If we want the exact rate then we'll have to reconfigure PLLs.

    // HSTX outputs 0 through 7 appear on GPIO 12 through 19.
    // Pinout on Pico DVI sock:
    //
    //   GP12 D0+  GP13 D0-
    //   GP14 CK+  GP15 CK-
    //   GP16 D2+  GP17 D2-
    //   GP18 D1+  GP19 D1-

    // Assign clock pair to two neighbouring pins:
    hstx_ctrl_hw->bit[2] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[3] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    for (uint lane = 0; lane < 3; ++lane) {
        // For each TMDS lane, assign it to the correct GPIO pair based on the
        // desired pinout:
        static const int lane_to_output_bit[3] = {0, 6, 4};
        int bit = lane_to_output_bit[lane];
        // Output even bits during first half of each HSTX cycle, and odd bits
        // during second half. The shifter advances by two bits each cycle.
        uint32_t lane_data_sel_bits =
            (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        // The two halves of each pair get identical data, but one pin is inverted.
        hstx_ctrl_hw->bit[bit    ] = lane_data_sel_bits;
        hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
    }

    for (int i = 12; i <= 19; ++i) {
        gpio_set_function(i, 0); // HSTX
    }

    // All channels are set up identically, to transfer a whole scanline and
    // then chain to the next channel. Each time a channel finishes, we
    // reconfigure the one that just finished, meanwhile the next channel
    // is already making progress.
    dma_channel_config c;

    for (uint32_t i = 0; i < DMACH_NUM; i++) {
        c = dma_channel_get_default_config(i);
        channel_config_set_chain_to(&c, (i+1) % DMACH_NUM);
        channel_config_set_high_priority(&c, true);
        channel_config_set_dreq(&c, DREQ_HSTX);
        dma_channel_configure(
            i,
            &c,
            &hstx_fifo_hw->fifo,
            vblank_line_vsync_off,
            count_of(vblank_line_vsync_off),
            false
        );
        dma_channel_claim(i);

        dma_hw->ints0 |= (1u << i);
        dma_hw->inte0 |= (1u << i);
    }
    
    irq_set_exclusive_handler(DMA_IRQ_NUM(DVI_DMA_IRQ), dma_irq_handler); //, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_NUM(DVI_DMA_IRQ), true);
    irq_set_priority(DMA_IRQ_NUM(DVI_DMA_IRQ), 4);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    dma_channel_start(0);
}

/* bv decoder */

#include <bv/bvdec.h>
#include "vid_file.h"

static uint32_t flash_seek = 0;

static struct bv_stream bv_s;
static uint8_t *bv_fbs[2];

static void print_frame() {
    const uint32_t y_step = bv_s.extent[1] / 8, x_step = bv_s.extent[0] / 32;
    const uint32_t row_step = bv_s.extent[0] / 8;

    for (uint32_t y = 0; y < bv_s.extent[1]; y += y_step) {
        for (uint32_t x = 0; x < bv_s.extent[0]; x += x_step) {
            uint8_t b = active_fb[x + y * bv_s.extent[0]];
            if (b) printf("#"); else printf(" ");
        }
        printf("\n");
    }

    printf("\n");
}

void step_video() {
    while (true) {
        int32_t res = bv_stream_decframe(&bv_s);

        if (res < 0) {
            uint32_t to_read = MIN(1024, bad_bv_len - flash_seek);
            assert(to_read);
            
            bv_stream_read(&bv_s, &bad_bv[flash_seek], to_read);
            flash_seek += to_read;
            
            continue;
        }
        
        print_frame();
        printf("v: rh %d bh %d f %d\n", bv_s.buf_head, bv_s.bit_head, bv_s.frame_index);
        
        for (uint32_t y = 0; y < 180; y++) {
            for (uint32_t x = 0; x < bv_s.extent[0]; x++) {
                uint32_t postscale_pixel = x * MODE_H_ACTIVE_PIXELS / bv_s.extent[0], postscale_size = (x + 1) * MODE_H_ACTIVE_PIXELS / bv_s.extent[0];
                memset(&dvi_fb[y * MODE_H_ACTIVE_PIXELS + postscale_pixel], active_fb[y * bv_s.extent[0] + x], postscale_size - postscale_pixel);
            }   
        }

        break;
    }

    return 0;
}

void setup_video() {
    bv_stream_init(&bv_s);

    bv_stream_read(&bv_s, bad_bv, 1024);
    flash_seek = 1024;

    bv_stream_configure(&bv_s);
    printf("bv_stream: ex %dx%d fr %d\n", bv_s.extent[0], bv_s.extent[1], bv_s.framerate);

    bv_fbs[0] = malloc(bv_s.fb_size);
    // bv_fbs[1] = malloc(bv_s.fb_size);
    memset(bv_fbs[0], 0, bv_s.fb_size);
    // memset(bv_fbs[1], 0, bv_s.fb_size);

    bv_stream_bind(&bv_s, bv_fbs);
    active_fb = bv_stream_active_fb(&bv_s);
}
