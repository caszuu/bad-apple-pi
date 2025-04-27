# Bad Apple Pi
A small project porting the entire [Bad Apple](https://www.youtube.com/watch?v=FtutLA63Cp8) to a Pico 2 without any requirement for an sd card or external storage.

## Hardware

This project relies on extra hardware only for its output. Currently the firmware image expects:
 - A vannila [DVI Sock](https://github.com/Wren6991/Pico-DVI-Sock) attached on gpio 12-19
 - A analogue [PWM circuit](https://datasheets.raspberrypi.com/rp2040/hardware-design-with-rp2040.pdf#%5B%7B%22num%22%3A26%2C%22gen%22%3A0%7D%2C%7B%22name%22%3A%22XYZ%22%7D%2C115%2C188.89192%2Cnull%5D) with left and right channels attached to gpios 0 and 1 respectively

## Building

To build the firmware image, first clone the repo and its dependencies
```sh
git clone https://github.com/caszuu/bad-apple-pi.git
cd bad-apple-pi/
git submodule update --init
```

This project uses a patched version of the `pico-extras` lib to enable stereo support, which we need to apply
```sh
git apply stereo.patch
```

and build the project using `cmake` and `ninja`
```sh
cmake -G Ninja -B build . -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

After the build is finished, the firmware will be at `build/ba/ba_image.uf2` or `build/ba/ba_image.elf`

## Credits

bad-apple-pi uses the following for its audio:
 - Raspberry Pi's [pico-extras](https://github.com/raspberrypi/pico-extras) - as a PIO PWM driver
 - Xiph's [libogg](https://gitlab.xiph.org/xiph/ogg.git) and [libvorbis](https://github.com/xiph/vorbis.git) - for its amazing audio codec

TBD:
- a lil Hacking Guide
- short showcase gif 
