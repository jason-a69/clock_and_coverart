

https://github.com/user-attachments/assets/7b6dc58a-73dd-4b02-bbbb-5923e1235b79





Program designed to run on pico2 w its main purpose is to display a clock and coverart on a waveshare 96x48 RGB Matrix

This would not have been possible without the excellent hub75 driver published at 

https://github.com/JuPfu/hub75/

Those 2 guys helped me a lot, getting the driver in shape for the Waveshare panel and offering invaluable guidance, I cannot thank them enough. I have used their code as a starting point for this project.

Hardware required :
- Pico2 W
- RGB Matrix Adapter Board (https://seengreat.com/product/194/rgb-matrix-adapter-board)
- Waveshare 96x48 RGB panel (I sourced mine from Taobao)
- Power supply (I sourced mine from a local shop in Taiwan) 

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

Yes, it's one hell of a faff to do without the adapter board.

For the clock to work properly, you need to give your Pico2 W access to the internet so that the internal clock can be updated using an NTP server. You also need to provide a timezone which in my case is CST-8 for Taiwan 🇹🇼

Create a file called cmake.secret after you have cloned this repository, this stores your WiFi and timezone information. The contents would typically be

```python
set(SECRET_WIFI_SSID "Your WiFi SSID")
set(SECRET_WIFI_PASSWORD "Your WiFi password")
set(SECRET_TX "CST-8")
```

See https://www.veron.nl/wp-content/uploads/2022/12/Posix-Timezone-Strings.pdf for your posix timezone string

Write this file in the top level clock_and_coverart directory.

There is another program that runs on a Raspberry Pi (I use a zero) which listens to the music that is playing on a Roon audio system, extracts the cover art, converts it to bmp and sends it over to this pico, the functionality is in this program to listen on tcp port 5000 and store the bmp which has been sent. The code for the Pi is at 

https://github.com/jason-a69/roon_cad/

The Pi needs to know the IP address of the Pico2 W which you can get from your router or by listening to the debug which is sent from the USB port. A future mod could be to assign a fixed IP address to make things more stable.

Once the bmp (coverart) has been received, it is displayed on the right hand side of the screen leaving the clock on the left hand side. If the music is stopped, the right hand side goes blank.






