import pygame
import struct
import bitarray.util as bitutil

from tqdm import tqdm, trange
from bitarray import bitarray
from dataclasses import dataclass

# == bvplay config ==

BV_INSPECT = False

# == decoder internals ==

@dataclass(slots=True)
class BitVState:
    bv_cursor: tuple[int, int]
    bv_table: tuple[int, bitarray]

    bv_extent: tuple[int, int]
    bv_framerate: int = 30

def bv_open_stream(path: str) -> tuple[BitVState, bitarray]:
    with open(path, 'rb') as f:
        if f.read(6) != b'BitV\x00\x00':
            raise IOError("file is not a BitV file")

        state_data = f.read(6)
        state_tuple = struct.unpack("<HHH", state_data)
        state = BitVState((0, 0), {}, state_tuple[0:2], state_tuple[2])

        bv_table_data = f.read(2 * 256)
        for i in range(256):
            tile = bitarray(endian='little')
            tile.frombytes(bv_table_data[i * 2: i * 2 + 2])
            
            state.bv_table[i] = tile

        stream = bitarray(endian='little')
        stream.fromfile(f)

        return (state, stream)

def bv_draw_supertile(bits: bitarray, state: BitVState, seek_head: int, win_buf: pygame.BufferProxy, win_surf) -> int:
    adjecency_prefix = bits[seek_head:seek_head + 2]
    cv_bitmask = bits[seek_head + 2:seek_head + 18]
    seek_head += 18

    base_tx, base_ty = (state.bv_cursor[0] * 4, state.bv_cursor[1] * 4)

    if BV_INSPECT:
        pygame.draw.rect(win_surf, (128, 128, 255), (base_tx * 4, base_ty * 4, 16, 16))

    for ty in range(4):
        for tx in range(4):
            if not cv_bitmask[tx + ty * 4]:
                continue

            cmd_b0 = bits[seek_head]

            if cmd_b0:
                # uniform tile, fill to surf

                polarity = bits[seek_head + 1]
                pygame.draw.rect(win_surf, (128, 255, 128) if BV_INSPECT else ((255, 255, 255) if polarity else (0, 0, 0)), ((base_tx + tx) * 4, (base_ty + ty) * 4, 4, 4))

                seek_head += 2

            else:
                # non-uniform tile, blit to surf

                cmd_b1 = bits[seek_head + 1]

                if cmd_b1:
                    # frequent tile, pull from bv_table

                    table_index = bitutil.ba2int(bits[seek_head + 2:seek_head + 10])
                    tile_data = state.bv_table[table_index]
                    seek_head += 10

                else:
                    # infrequent tile, blit inline data

                    tile_data = bits[seek_head + 2:seek_head + 18]
                    seek_head += 18

                base_index = (base_tx + tx) * 4 + (base_ty + ty) * 4 * state.bv_extent[0]

                if BV_INSPECT:
                    for y in range(4):
                        for x in range(4):
                            win_buf.write((b'\x8f\x8f\xff\xff' if not cmd_b1 else b'\x8f\xff\xff\xff') if tile_data[x + y * 4] else b'\x00\x00\x00\xff', (base_index + x + y * state.bv_extent[0]) * 4)

                else:
                    for y in range(4):
                        for x in range(4):
                            # pygame.draw.rect(win_surf, (255, 255, 255) if tile_data[0] else (0, 0, 0), ((base_tx + tx) * 4, (base_ty + ty) * 4, 4, 4))
                            # win_surf.set_at(((base_tx + tx) * 4 + x, (base_ty + ty) * 4 + y), (255, 255, 255) if tile_data[x + y * 4] else (0, 0, 0))
                            win_buf.write(b'\xff\xff\xff\xff' if tile_data[x + y * 4] else b'\x00\x00\x00\xff', (base_index + x + y * state.bv_extent[0]) * 4)

    if adjecency_prefix == bitarray("00"):
        state.bv_cursor = (state.bv_cursor[0] + 1, state.bv_cursor[1])
    elif adjecency_prefix == bitarray("01"):
        state.bv_cursor = (state.bv_cursor[0] - 1, state.bv_cursor[1])
    elif adjecency_prefix == bitarray("10"):
        state.bv_cursor = (state.bv_cursor[0], state.bv_cursor[1] + 1)
    else:
        state.bv_cursor = (state.bv_cursor[0], state.bv_cursor[1] - 1)

    return seek_head

def bv_play(state: BitVState, bits: bitarray) -> None:
    # init pygame player

    win_surf = pygame.display.set_mode(state.bv_extent, vsync=0)
    win_buf = win_surf.get_buffer()
    win_clk = pygame.time.Clock()

    pygame.display.set_caption("bvplay")

    frame_i = 0
    seek_head = 0
    last_seek_head = 0

    playback = True
   
    while True:
        # frame_bits = bits[(18 + state.bv_extent[0] * state.bv_extent[1]) * frame_i: (18 + state.bv_extent[0] * state.bv_extent[1]) * (frame_i + 1)]
        
        # # proc flip cmd
        # flip_offset = struct.unpack("<bb", bits[2:18].tobytes())
        # frame_bits <<= 2 + 16

        # win_surf.scroll(*flip_offset)

        # # proc frame "diff"
        # frame_data = b''.join(map(lambda px: b'\xff\xff\xff\xff' if px else b'\x00\x00\x00\xff', frame_bits[:state.bv_extent[0] * state.bv_extent[1]]))
        # win_surf.get_buffer().write(frame_data)

        cmd_b0 = bits[seek_head]

        if cmd_b0:
            # draw supertile
            seek_head += 1
            seek_head = bv_draw_supertile(bits, state, seek_head, win_buf, win_surf)

        else:
            cmd_b1 = bits[seek_head + 1]
            seek_head += 2

            if cmd_b1:
                # move cmd

                state.bv_cursor = (bitutil.ba2int(bits[seek_head:seek_head + 5]), bitutil.ba2int(bits[seek_head + 5: seek_head + 10]))
                seek_head += 10

            else:
                # flip cmd

                while True:
                    nextf = False
                    
                    for e in pygame.event.get():
                        if e.type == pygame.QUIT:
                            exit()

                        elif e.type == pygame.KEYDOWN and e.key == pygame.K_RIGHT:
                            nextf = True

                        elif e.type == pygame.KEYDOWN and e.key == pygame.K_SPACE:
                            playback ^= True

                    if nextf or playback:
                        break

                pygame.display.flip()
                win_clk.tick(state.bv_framerate)

                flip_offset = struct.unpack("<bb", bits[seek_head:seek_head + 16].tobytes())
                win_surf.scroll(dx=flip_offset[0])
                win_surf.scroll(dy=flip_offset[1])

                if BV_INSPECT:
                    win_surf.fill((0, 0, 0))

                print(f"f: {frame_i}; vb: {round((seek_head - last_seek_head) / 1024, 2)}kb/f")
                last_seek_head = seek_head

                frame_i += 1
                state.bv_cursor = (0, 0)
                seek_head += 16

# == bvplay frontend ==

if __name__ == '__main__':
    bv_state, bv_stream = bv_open_stream("out.bv")
    print(bv_state)

    bv_play(bv_state, bv_stream)
