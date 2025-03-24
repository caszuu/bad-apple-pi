#include <hardware/clocks.h>
#include <hardware/watchdog.h>
#include <pico/audio.h>
#include <pico/audio_pwm.h>
#include <pico/multicore.h>

#include <vorbis/codec.h>

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Vorbis -> PCM decoder state */

static ogg_sync_state o_sync;
static ogg_stream_state o_stream;
static ogg_page o_page;
static ogg_packet o_packet;

static vorbis_info vb_info;
static vorbis_comment vb_com;
static vorbis_dsp_state vb_dsp;
static vorbis_block vb_block;

static bool o_eos = false;

/* PCM -> PWM driver state */
static audio_buffer_pool_t *pcm_buffer_pool[2];
static struct producer_pool_blocking_give_connection pcm_connections[2];

const float pcm_volume = .7f;

#include "aud_file.h"
static uint32_t flash_seek = 0;

// flash -> ogg_sync
static void flash_pagein() {
    // if (o_eos)
    //     watchdog_reboot(0, 0, 0);

    uint8_t *buf = ogg_sync_buffer(&o_sync, 2048);

    uint32_t to_read = MIN(2048, bad_ogg_len - flash_seek);
    assert(to_read);

    memcpy(buf, bad_ogg + flash_seek, to_read);
    flash_seek += to_read;

    ogg_sync_wrote(&o_sync, to_read);
}

// ogg_sync -> ogg_stream
static void stream_pagein() {
    while (true) {
        int res = ogg_sync_pageout(&o_sync, &o_page);

        if (res == 0) {
            flash_pagein();
            continue;
        }
        assert(res > 0 && "corrupt ogg stream");

        break;
    }

    if (ogg_page_eos(&o_page))
        o_eos = true;
    ogg_stream_pagein(&o_stream, &o_page);
}

uint32_t step_audio() {
    float **vb_buf;
    int samples_in = vorbis_synthesis_pcmout(&vb_dsp, &vb_buf);

    if (samples_in > 0) {
        /* decoded pcm samples are buffered, give them to the pwm driver */

        int samples_out = MIN(samples_in, 1024);

        for (uint32_t ch = 0; ch < vb_info.channels; ch++) {
            audio_buffer_t *pwm_buf = take_audio_buffer(pcm_buffer_pool[ch], true);
            pwm_buf->sample_count = samples_out;

            float *in = vb_buf[ch];
            int16_t *out = (int16_t *)pwm_buf->buffer->bytes;

            for (uint32_t si = 0; si < samples_out; si++) {
                int32_t val = floorf(in[si] * 32767.f * pcm_volume + .5f);
                if (val > 32767) {
                    val = 32767;
                }
                if (val < -32768) {
                    val = -32768;
                }

                out[si] = val;
            }

            give_audio_buffer(pcm_buffer_pool[ch], pwm_buf);
        }

        printf("a: rh %d sc %d\n", flash_seek, samples_out);
        vorbis_synthesis_read(&vb_dsp, samples_out);
    } else {
        /* out of decoded pcm, syntetize a packet */

        while (true) {
            int res = ogg_stream_packetout(&o_stream, &o_packet);

            if (res == 0) {
                // need more data from ogg sync
                stream_pagein();
                continue;

            } else if (res < 0) {
                // missing or corrupt data, ignore as it should be catched at sync_pageout
                continue;
            }

            if (vorbis_synthesis(&vb_block, &o_packet) == 0) {
                // syntesis succeeded, append to syntesis_pcmout
                vorbis_synthesis_blockin(&vb_dsp, &vb_block);
                break;
            }
        }
    }
}

static void core1_loop() {
    // sync with core0
    // multicore_fifo_push_blocking(0);
    // multicore_fifo_pop_blocking();

    while (true) {
        uint32_t audio_us = step_audio();

        // multicore_fifo_push_blocking(audio_us);
    }
}

static void setup_bitstream() {
    /* submit initial buffer */

    flash_pagein();
    
    /* init stream */
    
    assert(ogg_sync_pageout(&o_sync, &o_page) == 1 && "not an ogg stream");
    ogg_stream_init(&o_stream, ogg_page_serialno(&o_page));
    
    /* extract vorbis header */

    vorbis_info_init(&vb_info);
    vorbis_comment_init(&vb_com);

    assert(ogg_stream_pagein(&o_stream, &o_page) >= 0);
    assert(ogg_stream_packetout(&o_stream, &o_packet) == 1);

    assert(vorbis_synthesis_headerin(&vb_info, &vb_com, &o_packet) >= 0 && "not a vorbis stream");

    for (uint32_t headers_read = 0; headers_read < 2;) {
        int res = ogg_sync_pageout(&o_sync, &o_page);

        if (res == 0) {
            // need more data from flash
            flash_pagein();
            continue;
        }

        if (res == 1) {
            ogg_stream_pagein(&o_stream, &o_page);

            while (headers_read < 2) {
                res = ogg_stream_packetout(&o_stream, &o_packet);

                if (res == 0)
                    break; // need more data from ogg sync
                if (res < 0)
                    assert(false && "vorbis header probably corrupt (pout)");

                res = vorbis_synthesis_headerin(&vb_info, &vb_com, &o_packet);
                if (res < 0)
                    assert(false && "vorbis header probably corrupt (syn)");

                headers_read++;
            }
        }
    }

    /* header read, print audio info and init dsp */

    printf("\n\nAudio is %d channel, %ldHz\n", vb_info.channels, vb_info.rate);
    printf("Encoded by: %s\n\n", vb_com.vendor);
    
    assert(vb_info.channels <= 2);
    
    assert(vorbis_synthesis_init(&vb_dsp, &vb_info) == 0 && "corrupt header during playback init");
    vorbis_block_init(&vb_dsp, &vb_block);
}

const audio_pwm_channel_config_t left_channel_config = {
    .core = {
        .base_pin = PICO_AUDIO_PWM_L_PIN,
        .pio_sm = 0,
        .dma_channel = 4
    },
    .pattern = 1,
};

const audio_pwm_channel_config_t right_channel_config = {
    .core = {
        .base_pin = PICO_AUDIO_PWM_R_PIN,
        .pio_sm = 1,
        .dma_channel = 5
    },
    .pattern = 1,
};

void setup_audio() {
    // init Vorbis decoder
    
    ogg_sync_init(&o_sync);
    setup_bitstream();

    // init PWM driver

    audio_format_t pcm_format = {
        .format = AUDIO_BUFFER_FORMAT_PCM_S16,
        .channel_count = vb_info.channels,
        .sample_freq = vb_info.rate, // match with pico_audio_pwm driver
    };

    audio_buffer_format_t pcm_buffer_format = {
        .format = &pcm_format,
        .sample_stride = sizeof(int16_t),
    };

    audio_pwm_setup(&pcm_format, -1, &left_channel_config, &right_channel_config);
    // audio_pwm_default_connect(pcm_buffer_pool, false);

    // connect pcm audio pools
    pcm_format.channel_count = 1;
    
    for (uint32_t ch = 0; ch < vb_info.channels; ch++) {
        pcm_buffer_pool[ch] = audio_new_producer_pool(&pcm_buffer_format, 3, 1024);
        audio_pwm_channel_connect(pcm_buffer_pool[ch], &pcm_connections[ch], ch);
    }
    
    audio_pwm_set_enabled(true);

    // audio_setup is already running on core1, continue to loop
    core1_loop();
}
