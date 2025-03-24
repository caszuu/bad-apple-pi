#pragma once
#include <stdint.h>

#define BV_TILESET_SIZE 256
#define BV_READ_BUF_SIZE 2048

struct __attribute__((__packed__)) bv_header {
    uint8_t __magic[6];

    uint16_t extent[2];
    uint16_t framerate;

    uint16_t tileset[256];
};

struct bv_stream {
    uint16_t extent[2];
    uint16_t framerate;

    uint32_t fb_size;
    uint16_t frame_index;
    uint8_t *fbs[2];

    uint16_t cursor[2];
    uint16_t tileset[BV_TILESET_SIZE];
    
    uint32_t bit_head;
    uint32_t buf_head;
    uint8_t read_buf[BV_READ_BUF_SIZE];
};