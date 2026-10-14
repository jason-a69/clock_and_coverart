Program designed to run on pico2w that will display a clock and coverart on a waveshare 96x48 RGB Matrix

This would not have been possible without the excellent hub75 driver published at 

https://github.com/JuPfu/hub75/

Those 2 guys helped me a lot, getting the driver in shape for the Waveshare panel and offering invaluable guidance, I cannot thank them enough. I have used their code as a starting point for this project.

Hardware required :
    Pico2W
    RGB Matrix Adapter Board (https://seengreat.com/product/194/rgb-matrix-adapter-board)
    Waveshare 96x48 RGB panel (I sourced mine from Taobao)
    Power bank 

The RGB Matrix Apapter Board is not essential but it really helps with the wiring. If you do use one then the pins will change in the code.

For example,with the RGB Matrix Apapter Board

    .pins = {
        .data_base_pin = 2,
        .data_n_pins = 6,
        .rowsel_base_pin = 8,
        .rowsel_n_pins = 3,
        .clk_pin = 13,
        .strobe_pin = 14,
        .oen_pin = 15,
    },

Without

    .pins = {
        .data_base_pin = 0,
        .data_n_pins = 6,
        .rowsel_base_pin = 6,
        .rowsel_n_pins = 3,
        .clk_pin = 11,
        .strobe_pin = 12,
        .oen_pin = 13,
    },

You need to refer to the Waveshare documentation on how to connect your panel to your Pico which is here

https://docs.waveshare.com/RGB-Matrix-Px-96x48/Raspberry-Pico





