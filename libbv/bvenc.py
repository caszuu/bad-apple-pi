import pygame
import struct
import bitarray.util as bitutil

from multiprocessing import Pool

from tqdm import tqdm, trange
from bitarray import bitarray, frozenbitarray
from dataclasses import dataclass

# == bv bit commands ==

BV_FLIP = bitarray("00")
BV_MOVE = bitarray("01")

BV_STILE = bitarray("1")

# == encoder internals ==

@dataclass(slots=True)
class BitVState:
    bv_extent: tuple[int, int]
    bv_framerate: int = 30

def _enc_quantize_image(path: str) -> tuple[tuple[int, int], bitarray]:
    # load image
    img = pygame.image.load(path)
    # img = pygame.transform.scale(img, bit_frame_extent)

    # quantize image to a bi-level bitmap
    bitframe = bitarray()

    for y in range(img.get_height()):
        for x in range(img.get_width()):
            val = img.get_at((x, y)).grayscale().r

            b = False
            if val > 192:
                b = True
            elif val > 128:
                b = (x + y) % 2 == 0
            elif val > 98:
                b = (x + y * 2) % 4 == 0
 
            bitframe.append(b)

    return (img.get_size(), bitframe)

# load source images from paths and returns uncompressed bitframes
def enc_quantize_image_seq(paths: list[str]) -> tuple[BitVState, list[bitarray]]:
    bit_frames = []
    bit_frame_extent = None

    worker_pool = Pool()

    for extent, bitframe in tqdm(worker_pool.imap(_enc_quantize_image, paths), desc="quatizing frames", unit="frames", total=len(paths)):
        if bit_frame_extent is None:
            bit_frame_extent = extent
        elif bit_frame_extent != extent:
            print(bit_frame_extent, extent)
            raise ValueError("all source images must have the same resolution.")

        bit_frames.append(bitframe)

    return (BitVState(bv_extent=bit_frame_extent), bit_frames)

def _enc_offset_frame(src_frame: bitarray, x: int, y: int, wh: tuple[int, int]) -> bitarray:
    frame = src_frame.copy()

    # offset x

    if x > 0:
        for row in range(wh[1]):
            frame[row * wh[0] + x:row * wh[0] + wh[0]] = frame[row * wh[0]: row * wh[0] + wh[0] - x]

    elif x < 0:
        x = -x

        for row in range(wh[1]):
            frame[row * wh[0]:row * wh[0] + wh[0] - x] = frame[row * wh[0] + x: row * wh[0] + wh[0]]

    # offset y

    if y > 0:
        extrapolated_rows = frame[:y * wh[0]]
        cropped_rows = frame[:wh[0] * (wh[1] - y)]

        frame = extrapolated_rows + cropped_rows
        
    elif y < 0:
        y = -y
        
        extrapolated_rows = frame[(wh[1] - y) * wh[0]:]
        cropped_rows = frame[y * wh[0]:]

        frame = cropped_rows + extrapolated_rows

    return frame

def _enc_estimate_motion(state: BitVState, prev_f: bitarray, curr_f: bitarray) -> tuple[int, int]:
    return (0, 0)# check frames over the max motion window
    min_err = float("inf")
    motion = None

    for y in range(-16, 16, 2):
        for x in range(-16, 16, 2):
            offset_prev_f = _enc_offset_frame(prev_f, x, y, state.bv_extent)

            err = bitutil.count_xor(offset_prev_f, curr_f)
            if err < min_err:
                min_err = err
                motion = (x, y)

    return motion

# tries to encode frame to frame motion, returns bv_flip commands with global motion info 
def enc_estimate_motion(state: BitVState, bit_frames: list[bitarray]) -> list[tuple[int, int]]:
    worker_pool = Pool()
    task_awaits = []

    # dispatch tasks to workers
    for frame_i in range(1, len(bit_frames)):
        prev_i = frame_i - 1

        prev_f = bit_frames[prev_i]
        curr_f = bit_frames[frame_i]

        task_awaits.append(worker_pool.apply_async(_enc_estimate_motion, (state, prev_f, curr_f)))

    # await dispatched tasks
    flips = []
    
    for task in tqdm(task_awaits, desc="estimating motion", unit="frames"):
        flips.append(task.get())

    return flips

def _enc_detect_reuse(src: bitarray, dst: bitarray, wh: tuple[int, int]) -> dict[frozenbitarray, int]:
    # == detect damaged tiles ==

    diff = src ^ dst
    damaged_supertiles = {}

    for y in range(wh[1]):
        for x in range(wh[0]):
            if diff[x + y * wh[0]]:
                damaged_tiles = damaged_supertiles.setdefault((x // 16, y // 16), set())
                damaged_tiles.add((x % 16 // 4, y % 16 // 4))

    # == count tile reuse ==

    tile_reuse_counts = {}

    for stile_loc, stile in damaged_supertiles.items():
        base_x, base_y = (stile_loc[0] * 16, stile_loc[1] * 16)
        for (tile_x, tile_y) in stile:
            tile_data = bitarray()
            for y in range(4):
                row_index = (base_x + tile_x * 4) + (base_y + tile_y * 4 + y) * wh[0]
                tile_data += dst[row_index:row_index + 4]

            tile_data = frozenbitarray(tile_data)

            if tile_data.all() or (not tile_data.any()):
                continue

            tile_reuse_counts.setdefault(tile_data, 0)
            tile_reuse_counts[tile_data] += 1

    return tile_reuse_counts

def enc_build_tile_set(state: BitVState, bit_frames: list[bitarray], flips: list[tuple[int, int]]) -> dict:
    worker_pool = Pool()
    task_awaits = []

    prev_f = bit_frames[0]

    # scan initial frame
    tile_counts = _enc_detect_reuse(bitarray(len(prev_f)), prev_f, state.bv_extent)

    # dispatch frame scan tasks
    for frame_i in range(1, len(bit_frames)):
        src_f = _enc_offset_frame(bit_frames[frame_i - 1], *flips[frame_i - 1], state.bv_extent)
        dst_f = bit_frames[frame_i]

        task_awaits.append(worker_pool.apply_async(_enc_detect_reuse, (src_f, dst_f, state.bv_extent)))

    # assemble final bitstream
    for frame_i in trange(1, len(bit_frames), desc="building tile sets", unit="frames"):
        # merge tile reuse counts
        for tile, count in task_awaits[frame_i - 1].get().items():
            tile_counts.setdefault(tile, 0)
            tile_counts[tile] += count

    # conv counts to a set of 256 most used tiles
    most_reused = sorted(tile_counts.items(), key=lambda item: item[1], reverse=True)[:min(len(tile_counts), 256)]

    tile_set = {}
    for i, (tile, _) in enumerate(most_reused):
        tile_set[tile] = i

    return tile_set

def _enc_encode_diff(src: bitarray, dst: bitarray, wh: tuple[int, int], tile_set: dict[frozenbitarray, int]) -> bitarray:
    # == encode tile diffs ==

    diff = src ^ dst
    bits = bitarray()
    damaged_supertiles = {}

    for y in range(wh[1]):
        for x in range(wh[0]):
            if diff[x + y * wh[0]]:
                damaged_tiles = damaged_supertiles.setdefault((x // 16, y // 16), set())
                damaged_tiles.add((x % 16 // 4, y % 16 // 4))

    cursor = (0, 0)

    def move_to_next_supertile(bits: bitarray) -> tuple[int, int]:
        cmd_bits = BV_MOVE.copy()
        next_supertile_loc = next(iter(damaged_supertiles))

        cmd_bits += bitutil.int2ba(next_supertile_loc[0], 5, endian='little')
        cmd_bits += bitutil.int2ba(next_supertile_loc[1], 5, endian='little')

        bits += cmd_bits
        return next_supertile_loc

    def draw_supertile(bits: bitarray, damaged_tiles: set, adjecency_prefix: bitarray) -> None:
        cmd_bits = BV_STILE + adjecency_prefix
        tile_bits = bitarray()

        cv_bitmask = bitarray(16)
        base_x, base_y = (cursor[0] * 16, cursor[1] * 16)

        for tile_y in range(4):
            for tile_x in range(4):
                if not (tile_x, tile_y) in damaged_tiles:
                    continue

                cv_bitmask[tile_x + tile_y * 4] = True

                tile_data = bitarray()
                for y in range(4):
                    row_index = (base_x + tile_x * 4) + (base_y + tile_y * 4 + y) * wh[0]
                    tile_data += dst[row_index:row_index + 4]

                tile_data = frozenbitarray(tile_data)

                # check if uniform
                
                if tile_data.all():
                    tile_bits += bitarray("11")

                elif not tile_data.any():
                    tile_bits += bitarray("10")

                # fallback to inline data for non-uniform

                else:
                    if tile_data in tile_set:
                        # frequent tile, index into tile set
                        tile_bits += bitarray("01") + bitutil.int2ba(tile_set[tile_data], 8, endian='little')

                    else:
                        # infrequent tile, inline full 16 bits
                        tile_bits += bitarray("00") + tile_data

        bits += cmd_bits + cv_bitmask + tile_bits

    while damaged_supertiles:
        if not cursor in damaged_supertiles:
            cursor = move_to_next_supertile(bits)

        tiles = damaged_supertiles.pop(cursor)
        
        # check for adjecent supertiles
        if (cursor[0] + 1, cursor[1]) in damaged_supertiles:
            draw_supertile(bits, tiles, bitarray("00"))
            cursor = (cursor[0] + 1, cursor[1])

        elif (cursor[0] - 1, cursor[1]) in damaged_supertiles:
            draw_supertile(bits, tiles, bitarray("01"))
            cursor = (cursor[0] - 1, cursor[1])

        elif (cursor[0], cursor[1] + 1) in damaged_supertiles:
            draw_supertile(bits, tiles, bitarray("10"))
            cursor = (cursor[0], cursor[1] + 1)

        else:
            # no matter if adjecent tile is bellow *or* not, move down, if no tile is bellow a move cmd will follow
            draw_supertile(bits, tiles, bitarray("11"))
            cursor = (cursor[0], cursor[1] - 1)

    # == encode feedback ==

    print(f"comp {round(len(bits) / len(dst) * 100, 2)}; base {len(dst)}; codec {len(bits)}")
  
    return bits

def enc_encode_frames(state: BitVState, bit_frames: list[bitarray], flips: list[tuple[int, int]], tile_set: dict[frozenbitarray, int]) -> bitarray:
    bits = bitarray()
    prev_f = bit_frames[0]

    worker_pool = Pool()
    task_awaits = []

    # insert bitstream tile set
    for t in tile_set.keys():
        bits += t

    # write initial frame
    bits += _enc_encode_diff(bitarray(len(prev_f)), prev_f, state.bv_extent, tile_set)

    # dispatch frame diff tasks
    for frame_i in range(1, len(bit_frames)):
        src_f = _enc_offset_frame(bit_frames[frame_i - 1], *flips[frame_i - 1], state.bv_extent)
        dst_f = bit_frames[frame_i]
        
        task_awaits.append(worker_pool.apply_async(_enc_encode_diff, (src_f, dst_f, state.bv_extent, tile_set)))

    # assemble final bitstream
    for frame_i in trange(1, len(bit_frames), desc="encoding frames", unit="frames"):
        # write flip cmd
        flip_cmd = BV_FLIP.copy()
        flip_cmd += bitutil.int2ba(flips[frame_i - 1][0], 8, endian='little', signed=True)
        flip_cmd += bitutil.int2ba(flips[frame_i - 1][1], 8, endian='little', signed=True)

        bits += flip_cmd
        
        # write frame diff
        bits += task_awaits[frame_i - 1].get()

    return bits

def enc_output_stream(state: BitVState, bv_bitstream: bitarray) -> bitarray:
    bitstream = bitarray(endian='little')

    # write stream header

    bv_header = b'BitV\x00\x00'
    bv_header += struct.pack("<HHH", state.bv_extent[0], state.bv_extent[1], state.bv_framerate)

    bitstream.frombytes(bv_header)
    
    # write bitframes

    bitstream += bv_bitstream

    with open("out.bv", 'wb') as f:
        bitstream.tofile(f)

# == libbv frontend ==

if __name__ == '__main__':
    paths = []

    import os
    for i in range(1, 10000): #range(1800, 2100):
        path = f"ba_vid/out{i:0=4}.png"
        if not os.path.exists(path):
            break

        paths.append(path)
    
    bv_state, bv_bitframes = enc_quantize_image_seq(paths)
    bv_flips = enc_estimate_motion(bv_state, bv_bitframes)
    bv_set = enc_build_tile_set(bv_state, bv_bitframes, bv_flips)
    bv_stream = enc_encode_frames(bv_state, bv_bitframes, bv_flips, bv_set)
    
    print(bv_state)

    enc_output_stream(bv_state, bv_stream)
