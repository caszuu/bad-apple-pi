add_executable(ba_image
    entry.c

    vid_core.c
    aud_core.c    
)

pico_set_program_name(ba_image "【東方】Bad Apple!! ＰＶ【影絵】")
pico_set_program_version(ba_image "2009 - あにら - EXSERENS feat. nomico")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(ba_image 0)
pico_enable_stdio_usb(ba_image 1)

# Add the standard library to the build
target_link_libraries(ba_image
        pico_stdlib
        pico_stdio)

# Add pico video libs and deps
target_link_libraries(ba_image
        bv
        
        hardware_dma
        hardware_irq
        hardware_clocks)

# Add pico audio drivers
target_link_libraries(ba_image
        pico_audio
        pico_audio_pwm

        Ogg::ogg
        Vorbis::vorbis
)

# Add the standard include files to the build
target_include_directories(ba_image PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/.. # for our common lwipopts or any other standard includes, if required
)

# Add any user requested libraries
target_link_libraries(ba_image 
    hardware_dma
    hardware_pio
    hardware_interp
    hardware_watchdog
    hardware_clocks

    pico_bootrom
    pico_multicore
)

pico_add_extra_outputs(ba_image)

