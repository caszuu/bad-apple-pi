#pragma once
#include "bv_structs.h"

#include <stdint.h>

/* bv_stream dec api */

// bvdec bv_stream init 
void bv_stream_init(struct bv_stream *s);
void bv_stream_deinit(struct bv_stream *s);

// read-in a bv header; returns: 0 - success, -1 - read needed
int32_t bv_stream_configure(struct bv_stream *s);

// find an externally owned framebuffer (of size at least s->fb_size)
// TODO: double-buffering
void bv_stream_bind(struct bv_stream *s, uint8_t *fbs[1]);

// bitstream read-in
void bv_stream_read(struct bv_stream *s, uint8_t *buf, uint32_t bytes_read);

// streaming decode; returns: 0 - success / frame done, -1 - read needed
int32_t bv_stream_decframe(struct bv_stream *s);

// fetch the currently active fb (aka fb which should be on-screen)
uint8_t* bv_stream_active_fb(struct bv_stream *s);