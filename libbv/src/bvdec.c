#include <bv/bvdec.h>

#include <pico/stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

void bv_stream_init(struct bv_stream *s) {
    memset(s, 0, sizeof(*s));
}

void bv_stream_deinit(struct bv_stream *s) {
}

/* bvdec read-in */

void bv_stream_read(struct bv_stream *s, uint8_t *buf, uint32_t bytes_read) {
    // validate

    assert(s->buf_head + bytes_read - MIN(s->buf_head + bytes_read, BV_READ_BUF_SIZE) <= s->bit_head / 8);

    // read-in

    uint32_t next_head = s->buf_head + bytes_read;
    if (next_head / BV_READ_BUF_SIZE > s->buf_head / BV_READ_BUF_SIZE) {
        uint32_t curr_head = s->buf_head % BV_READ_BUF_SIZE;
        next_head %= BV_READ_BUF_SIZE;

        uint32_t bytes_until_wrap = bytes_read - next_head;

        if (bytes_until_wrap)
            memcpy(&s->read_buf[curr_head], buf, bytes_until_wrap);
        if (next_head)
            memcpy(&s->read_buf[0], buf + bytes_until_wrap, next_head);
    } else {
        memcpy(&s->read_buf[s->buf_head % BV_READ_BUF_SIZE], buf, bytes_read);
    }

    s->buf_head += bytes_read;
}

static int32_t read_in_bytes(struct bv_stream *s, uint8_t *buf, uint32_t head, uint32_t size) {
    // validate

    assert(head + BV_READ_BUF_SIZE >= s->buf_head);

    uint32_t next_head = head + size;
    if (next_head >= s->buf_head)
        return -1; // read needed

    // read-in

    if (next_head / BV_READ_BUF_SIZE > head / BV_READ_BUF_SIZE) {
        head %= BV_READ_BUF_SIZE;
        next_head %= BV_READ_BUF_SIZE;

        uint32_t size_until_wrap = size - next_head;

        if (size_until_wrap)
            memcpy(buf, &s->read_buf[head], size_until_wrap);
        if (next_head)
            memcpy(buf + size_until_wrap, &s->read_buf[0], next_head);
    } else {
        head %= BV_READ_BUF_SIZE;
        memcpy(buf, &s->read_buf[head], size);
    }

    return 0;
}

// Treating buf[] as a giant little-endian integer, grab "width"
// bits starting at bit number "pos" (LSB=bit 0).
static uint32_t read_in_bits(struct bv_stream *s, int32_t *buf, size_t pos, uint32_t width) {
    assert(width >= 0 && width <= 32 - 7);

    // Read a 32-bit little-endian number starting from the byte
    // containing bit number "pos" (relative to "buf").
    uint32_t bits;
    int32_t res = read_in_bytes(s, (uint8_t*)&bits, pos / 8, sizeof(uint32_t));
    if (res < 0)
        return res;

    // Shift out the bits inside the first byte that we've
    // already consumed.
    // After this, the LSB of our bit field is in the LSB of bits.
    bits >>= pos % 8;

    // Return the low "width" bits, zeroing the rest via bit mask.
    *buf = bits & ((1ull << width) - 1);
    return 0;
}

/* bvdec config */

int32_t bv_stream_configure(struct bv_stream *s) {
    struct bv_header header_buf;
    int32_t res = read_in_bytes(s, &header_buf, 0, sizeof(struct bv_header));
    if (res < 0)
        return res;

    s->bit_head += sizeof(struct bv_header) * 8;

    // config bv_stream from header
    memcpy(s->extent, header_buf.extent, sizeof(header_buf.extent));
    memcpy(s->tileset, header_buf.tileset, sizeof(header_buf.tileset));
    s->framerate = header_buf.framerate;

    s->fb_size = (uint32_t)header_buf.extent[0] * (uint32_t)header_buf.extent[1];

    return 0;
}

void bv_stream_bind(struct bv_stream *s, uint8_t *fbs[1]) {
    memcpy(s->fbs, fbs, sizeof(uint8_t *) * 1);
}

/* bvdec streaming decode */

static int32_t draw_supertile(struct bv_stream *s, uint8_t* fb) {
    uint32_t local_head = s->bit_head + 1;
    
    uint32_t st_bits;
    int32_t res = read_in_bits(s, &st_bits, local_head, 18);
    if (res < 0)
        return res;
    local_head += 18;

    const uint8_t adj_prefix = st_bits & 3;
    const uint16_t cv_mask = st_bits >> 2;

    const uint32_t base_tx = s->cursor[0] * 16, base_ty = s->cursor[1] * 16;

    for (uint32_t ty = 0; ty < 4; ty++) {
        for (uint32_t tx = 0; tx < 4; tx++) {
            if (!((cv_mask >> (tx + ty * 4)) & 1)) continue;
            
            uint32_t cmd_bits;
            res = read_in_bits(s, &cmd_bits, local_head, 2);
            if (res < 0)
            return res;
            
            if (cmd_bits & 1) {
                // uniform tile
                bool polarity = cmd_bits & 2;

                /* uint32_t x = base_tx + tx * (4 / 2);
                uint32_t y = base_ty + ty * (4 / 2);
                uint32_t index = x + y * row_step;
                uint32_t col_shift = (tx % 2) * 4;
                uint8_t wr_mask = 15 << 4;
                uint8_t data = 15 * polarity;

                for (uint32_t row = 0; row < 4; row++) {
                    fb[index] &= wr_mask >> col_shift;
                    fb[index] |= data << col_shift;
                    index += row_step;
                } */

                for (uint32_t y = 0; y < 4; y++) {
                    for (uint32_t x = 0; x < 4; x++) {
                        uint32_t index = (base_tx + tx * 4 + x) + (base_ty + ty * 4 + y) * s->extent[0];
                        assert(index < s->extent[0] * s->extent[1]);
                        fb[index] = polarity ? 0xff : 0x00;
                    }
                }

                local_head += 2;
            } else if (cmd_bits & 2) {
                // indexed tile
                uint32_t index_bits;
                res = read_in_bits(s, &index_bits, local_head + 2, 8);
                if (res < 0)
                    return res;

                uint16_t tile_bits = s->tileset[index_bits & 255];
                
                /* uint32_t x = base_tx + tx * (4 / 2);
                uint32_t y = base_ty + ty * (4 / 2);
                uint32_t index = x + y * row_step;
                uint32_t col_shift = (tx % 2) * 4;
                uint8_t wr_mask = 15 << 4;

                for (uint32_t row = 0; row < 4; row++) {
                    fb[index] &= wr_mask >> col_shift;
                    fb[index] |= ((tile_bits >> (row * 4)) & 4) << col_shift;
                    index += row_step;
                } */

                for (uint32_t y = 0; y < 4; y++) {
                    for (uint32_t x = 0; x < 4; x++) {
                        uint32_t index = (base_tx + tx * 4 + x) + (base_ty + ty * 4 + y) * s->extent[0];
                        assert(index < s->extent[0] * s->extent[1]);
                        fb[index] = (tile_bits >> (x + y * 4)) & 1 ? 0xff : 0x00;
                    }
                }

                local_head += 10;
            } else {
                // inline tile
                uint32_t tile_bits;
                res = read_in_bits(s, &tile_bits, local_head + 2, 16);
                if (res < 0)
                    return res;

                /* uint32_t x = base_tx + tx * (4 / 2);
                uint32_t y = base_ty + ty * (4 / 2);
                uint32_t index = x + y * row_step;
                uint32_t col_shift = (tx % 2) * 4;
                uint8_t wr_mask = 15 << 4;

                for (uint32_t row = 0; row < 4; row++) {
                    fb[index] &= wr_mask >> col_shift;
                    fb[index] |= ((tile_bits >> (row * 4)) & 4) << col_shift;
                    index += row_step;
                } */

                for (uint32_t y = 0; y < 4; y++) {
                    for (uint32_t x = 0; x < 4; x++) {
                        uint32_t index = (base_tx + tx * 4 + x) + (base_ty + ty * 4 + y) * s->extent[0];
                        assert(index < s->extent[0] * s->extent[1]);
                        fb[index] = (tile_bits >> (x + y * 4)) & 1 ? 0xff : 0x00;
                    }
                }
                
                local_head += 18;
            }
        }
    }

    // successfully drawn supertile, can't fail now on
    s->bit_head = local_head;

    switch (adj_prefix) {
    case 0:
        s->cursor[0] += 1;
        break;

    case 2:
        s->cursor[0] -= 1;
        break;

    case 1:
        s->cursor[1] += 1;
        break;

    case 3:
        s->cursor[1] -= 1;
        break;
    }

    return 0;
}

static void shift_fb(struct bv_stream *s, uint8_t* fb, int8_t x, int8_t y) {
    // offset x
    
    if (x > 0) {
        for (uint32_t row = 0; row < s->extent[1]; row++)
            memmove(&fb[row * s->extent[0] + x], &fb[row * s->extent[0]], s->extent[0] - x);
    } else if (x < 0) {
        for (uint32_t row = 0; row < s->extent[1]; row++)
            memmove(&fb[row * s->extent[0]], &fb[row * s->extent[0] - x], s->extent[0] + x);
    }

    // offset y
    
    if (y > 0) {
        memmove(&fb[y * s->extent[0]], &fb[0], s->extent[0] * (s->extent[1] - y));
    } else if (y < 0) {
        memmove(&fb[0], &fb[-y * s->extent[0]], s->extent[0] * (s->extent[1] + y));
    }
}

int32_t bv_stream_decframe(struct bv_stream *s) {
    uint8_t* fb = s->fbs[0];
    assert(fb);
    
    // memcpy(fb, prev_fb, s->fb_size);

    int32_t res;
    while (true) {
        uint32_t cmd_bits;
        res = read_in_bits(s, &cmd_bits, s->bit_head, 2);
        if (res < 0)
            return res;

        if (cmd_bits & 1) {
            // supertile cmd
            res = draw_supertile(s, fb);
            if (res < 0)
                return res;

        } else if (cmd_bits & 2) {
            // move cmd
            uint32_t move_bits;
            res = read_in_bits(s, &move_bits, s->bit_head + 2, 10);
            if (res < 0)
                return res;

            s->cursor[0] = (move_bits >> 0) & 31;
            s->cursor[1] = (move_bits >> 5) & 31;

            s->bit_head += 12;

        } else {
            // flip cmd
            uint32_t flip_bits;
            res = read_in_bits(s, &flip_bits, s->bit_head + 2, 16);

            int8_t x_shift = ((int8_t *)&flip_bits)[0];
            int8_t y_shift = ((int8_t *)&flip_bits)[1];
            shift_fb(s, fb, x_shift, y_shift);

            s->bit_head += 18;
            break;
        }
    }

    memset(s->cursor, 0, sizeof(s->cursor));
    s->frame_index += 1;

    return 0;
}

uint8_t *bv_stream_active_fb(struct bv_stream *s) {
    uint8_t *fb = s->fbs[0]; // s->fbs[(s->frame_index - 1u) % 2];
    assert(fb);

    return fb;
}