#include "Config.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"

// Pico W devices use a GPIO on the WIFI chip for the LED,
// so when building for Pico W, CYW43_WL_GPIO_LED_PIN will be defined
#ifdef CYW43_WL_GPIO_LED_PIN
#include "pico/cyw43_arch.h"
#endif

#include "hub75.hpp"
#include "antialiased_line.hpp"

#include "pico/aon_timer.h"

//#include "antialiased_line.hpp"

using namespace pimoroni;
static PicoGraphics_PenRGB888 graphics(96, 48, nullptr);

#ifndef M_PI
#define M_PI (3.14159265358979323846)
#endif

// Define these parameters in a file called config.txt

//const char* WIFI_SSID = std::getenv("WIFI_SSID");
//const char* WIFI_PASSWORD = std::getenv("WIFI_PASSWORD");
//const char* TIMEZONE_OFFSET_HOURS = std::getenv("TIMEZONE_OFFSET_HOURS");



#include <stdio.h>
#include <stdlib.h>     // needed for setenv(), although also included by lwIP
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/apps/sntp.h"
#include "pico/util/datetime.h"
#include "pico/aon_timer.h"
#include "pico/mutex.h"
#include "lwip/tcp.h"
#include <cstring>


// create a mutex to avoid reading the aon_timer at the same time as lwIP/SNTP is updating it 
auto_init_mutex(aon_timer_mutex);
static bool aon_timer_is_initialised = false;

#define TCP_PORT 5000
#define IMG_SIZE (96 * 48 * 3)   // RGB888

static uint8_t image_buf[IMG_SIZE];
static size_t  recv_len = 0;
static bool    image_ready = false;

struct TCP_SERVER_T {
    struct tcp_pcb *server_pcb;
    struct tcp_pcb *client_pcb;
};

#define TIMEZONE_OFFSET_HOURS (+8) // Change to your local UTC offset (e.g. -5 for EST, +1 for CET)


// Full panel size
constexpr int PANEL_W = 96;
constexpr int PANEL_H = 48;
constexpr int IMG_W   = 48;
constexpr int IMG_H   = 48;
constexpr int OFFSET_X = 48;   // right side
constexpr int OFFSET_Y = 0;

// Full-screen BGR888 buffer (required by update_bgr)
static uint8_t screen_buf[PANEL_W * PANEL_H * 3];

// Callback for lwIP/SNTP to set the aon_timer to UTC
// We configure SNTP to call this function when it receives a valid NTP timestamp
// (see lwipopts.h)

// Panel/pin/color/rotation configuration - see include/hub75.hpp for field docs.
// Matches a single generic 64x64 panel wired to GPIO 0-13.
constexpr Hub75Config panel_cfg{
    .panel = {
        .matrix_panel_width = 96,
        .matrix_panel_height = 48,
        .chain_rows = 1,
        .chain_cols = 1,
        .chain_mode = Hub75ChainMode::SERPENTINE,
        .panel_kind = RowMapping::Standard,
        .address_type = RowAddressing::ABCShiftRegister,
        .scan_mode = 24, // 0: try to automatically deduce scan_mode - <value>: take value as scan_mode
        .panel_chip = Hub75PanelChip::GENERIC,
        .inverted_stb = false,
        .sm_clockdiv_factor = 3.0f,
        .base_latch_ns = 180,
        .base_addr_ns = 260,
    },
    .screen = {
        .rotation = Hub75Rotation::DEG_0,
    },
    .pins = {
        .data_base_pin = 2,
        .data_n_pins = 6,
        .rowsel_base_pin = 8,
        .rowsel_n_pins = 3,
        .clk_pin = 13,
        .strobe_pin = 14,
        .oen_pin = 15,
    },
    .color = {
        .bitplanes = 10,
        .separate_cie_channels = true,
        .balanced_light_output = true,
        .swap_rb_pins = true,
        .ccm_rg_shift = 6,
        .ccm_gb_shift = 7,
    },
    .frame_rate_debug = true,
};

using Panel = Hub75Driver<panel_cfg>;
// Large fixed-size buffers - must have static storage duration, not live on the stack.
static Panel driver;


// Perform initialisation
int pico_led_init(void)
{
#if defined(PICO_DEFAULT_LED_PIN)
    // A device like Pico that uses a GPIO for the LED will define PICO_DEFAULT_LED_PIN
    // so we can use normal GPIO functionality to turn the led on and off
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    return PICO_OK;
#elif defined(CYW43_WL_GPIO_LED_PIN)
    // For Pico W devices we need to initialise the driver etc
    return cyw43_arch_init();
#else
    return PICO_OK;
#endif
}

// Turn the led on or off
void pico_set_led(bool led_on)
{
#if defined(PICO_DEFAULT_LED_PIN)
    // Just set the GPIO on or off
    gpio_put(PICO_DEFAULT_LED_PIN, led_on);
#elif defined(CYW43_WL_GPIO_LED_PIN)
    // Ask the wifi "driver" to set the GPIO on or off
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on);
#endif
}

// Pico - please, blink LED when program starts
int led_init(void)
{
    int rc = pico_led_init(); // Initialize the LED
    hard_assert(rc == PICO_OK);

    for (int i = 0; i < 8; i++)
    {
        pico_set_led(true);
        sleep_ms(250); // Wait 250ms
        pico_set_led(false);
        sleep_ms(250); // Wait 250ms
    }
    return PICO_OK;
}


void core1_entry()
{
    driver.create();
    driver.start();

    // KEEP CORE 1 ALIVE — without this, Core 1's NVIC is torn down and DMA_IRQ_1 stops firing
    //
    // Add your additional tasks for core1 here
    while (true)
    {
        tight_loop_contents();
    }
}

// This is called from lwipopts.h
void sntp_set_system_time_us(uint32_t sec, uint32_t us) {    
    static struct timespec ntp_ts;
    // Calculate the offset required for where we are
    ntp_ts.tv_sec = sec + (3600 * TIMEZONE_OFFSET_HOURS);
    ntp_ts.tv_nsec = us * 1000;

    if (aon_timer_is_initialised) {
        // Wait up to 10ms to obtain exclusive access to the aon_timer
        if (mutex_enter_timeout_ms (&aon_timer_mutex, 10)) {
            aon_timer_set_time(&ntp_ts);
            mutex_exit(&aon_timer_mutex);   // release the mutex as soon as possible
            puts("-> Updated system time from NTP");
        } else {
            puts("-> Skipped NTP system time update (aon_timer was busy)");
        }
    } else {
        // The aon_timer is uninitialised so we don't need exclusive access
        aon_timer_is_initialised = aon_timer_start(&ntp_ts);
        puts("-> initialised system time from NTP");
    }
}

// This is called from lwipopts.h
// Callback for lwIP/SNTP to read system time (UTC) from the aon_timer
// We configure SNTP to call this function to read the current UTC system time,
// E.g. to calculate the roundtrip transmission delay (see lwipopts.h)
void sntp_get_system_time_us(uint32_t *sec_ptr, uint32_t * us_ptr) {
    static struct timespec sys_ts;
    // We don't need exclusive access because we are on the background thread
    aon_timer_get_time(&sys_ts);
    *sec_ptr = sys_ts.tv_sec;
    *us_ptr = sys_ts.tv_nsec / 1000;
}

// This is where we received a bmp file from the Pi
// Roon audio specifics :
//  Check the zone we have selected is playing, if it is send over a bmp file of the cover
//  art which is received here.
//  If the zone stops playing then a blank bmp is sent so that the right side of the screen
//  is blanked out leaving only the clock
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (!p) {
        // connection closed
        printf("In tcp_server_recv - connection closed");
        tcp_close(tpcb);
        return ERR_OK;
    }

    // Copy into our image buffer
    size_t copy_len = p->tot_len;
    if (recv_len + copy_len > sizeof(image_buf) + 8)  // +8 for header
        copy_len = sizeof(image_buf) + 8 - recv_len;

    pbuf_copy_partial(p, image_buf + recv_len, copy_len, 0);
    recv_len += copy_len;
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    // Simple protocol: first 8 bytes = "BMP1" + uint32_t size
    if (recv_len >= 8) {
        if (memcmp(image_buf, "BMP1", 4) == 0) {
            uint32_t expected;
            memcpy(&expected, image_buf + 4, 4);
            if (recv_len >= 8 + expected) {
                // image complete – skip the 8-byte header
                memmove(image_buf, image_buf + 8, expected);
                printf("Received an image");
                image_ready = true;
                recv_len = 0;          // ready for next image
            }
        }
    }
    return ERR_OK;
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    if (err != ERR_OK || client_pcb == NULL) {
        printf("Returning, error in tcp_server_accept\n");
        return ERR_VAL;
    }
    tcp_recv(client_pcb, tcp_server_recv);
    return ERR_OK;
}

static bool tcp_server_open() {
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    printf("In tcp_server_open\n");

    if (!pcb) {
        printf("PCB error in tcp_server_open\n");
        return false;
    }
    err_t err = tcp_bind(pcb, IP_ANY_TYPE, TCP_PORT);
    if (err) {
        printf("tcp bind error in tcp_server_open %s\n",err);
        return false;
    }
     
    struct tcp_pcb *server = tcp_listen_with_backlog(pcb, 1);
    if (!server) return false;
    tcp_accept(server, tcp_server_accept);
    return true;
}

void initialize()
{

    stdio_init_all(); // Initialize Pico SDK

    led_init(); // Initialize LED - blinking at program start

    // Core0 is used to Wi-Fi and processing
    // Core1 is reserved for the panel driver

    multicore_reset_core1();             // Reset core 1
    multicore_launch_core1(core1_entry); // Launch core 1 entry function - the Hub75 driver is doing its job there

    // Enable wifi station mode
    cyw43_arch_enable_sta_mode();
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
   
    // Define connection parameters
    uint32_t connection_timeout_ms = 30000;

    printf("Connecting to Wi-Fi...\n");
    printf("Wifi SSID is %s\n", WIFI_SSID);
    printf("Wifi password is %s\n", WIFI_PASSWORD);
    int status = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, connection_timeout_ms);
    if (status != 0) {
        printf("Wifi error in connecting is ", status);
    }

    // display the ip address in human readable form
    uint8_t *ip_address = (uint8_t*)&(netif_default->ip_addr.addr);
    printf("IP address %d.%d.%d.%d\n", ip_address[0], ip_address[1], ip_address[2], ip_address[3]);

    // Open the TCP server so we can receive bmp files
    if (!tcp_server_open()) {
        printf("TCP server failed\n");
    } else {
        printf("Listening on port %d\n", TCP_PORT);
    }

    // initialise the lwIP/SNTP application
    sntp_setoperatingmode(SNTP_OPMODE_POLL);        // lwIP/SNTP also accepts SNTP_OPMODE_LISTENONLY
    sntp_init();
}

template <uint32_t W, uint32_t H>
class AnalogClock : public AntialiasedLine<W, H>
{
private:
    float r = 1.0f;
    uint l = 1;

    float da = M_PI / 30.0f;

    float centerX = 0.0f;
    float centerY = 0.0f;

    float s[60];
    float c[60];

    int hour = 0;
    int minute = 0;
    int second = 0;

    int previous_second = second;

    int textScrollX = 64;

    constexpr static uint32_t hour_hand = 0x1bb1e7;          // #1bb1e7
    constexpr static int32_t minute_hand = 0x11819d;         // #11819d
    constexpr static uint32_t second_hand = 0xF3D459;        // #F3D459
    constexpr static uint32_t second_shadow_hand = 0x7a6203; // #7a6203

    constexpr static uint32_t shadow_hand = 0xffffff;  // #ffffff
    constexpr static uint32_t main_ticks = 0xffffff;   // #ffffff
    constexpr static uint32_t clock_digits = 0x7A263A; // #7A263A

public:
    explicit AnalogClock()
    {
        //get_time();

        centerX = (float)(W / 2);
        centerY = (float)(H / 2);

        l = std::min(W, H);


        r = l * (0.95f / 2.0f);

        for (auto i = 15; i < 75; i++)
        {
            s[i - 15] = std::sin(i * da) * r;
            c[i - 15] = std::cos(i * da) * r;
        }

        struct timespec ts = {0LL, 0L};
        ts.tv_sec = 60 * 60 * 3 + 25 * 60 + 45;

        if (!aon_timer_start(&ts))
        {
            printf("COULD NOT SET AON TIMER");
        }

        this->set_font(&font14_outline);
        //this->set_font(&font8);

    }

    void draw()
    {
        struct timespec ct;
        if (aon_timer_get_time(&ct))
        {
            int period = (ct.tv_sec % (3600 * 12));
            hour = (ct.tv_sec % (3600 * 12)) / 3600;
            minute = (period - hour * 3600) / 60;
            second = (period - hour * 3600 - minute * 60);
        }

        // only update frame buffer when second has changed
        if (second != previous_second)
        {
            previous_second = second;
            this->set_pen(0);


            // Clear down the left side of the screen
            for (int x = 0; x < 48; x++) {
                for (int y = 0; y < 48; y++) {
                    this->set_pen(0);
                    this->set_pixel(Point(x, y)); 
                }
            }
            //this->clear();

            this->set_pen(clock_digits);

            int shiftLeft = 26; 
            int localCenterX = centerX - shiftLeft;

            if constexpr (W > 32)
            {   

                 // Subtracted shiftLeft from the X coordinates of the numbers
                this->text("12", Point(((W) / 2 - shiftLeft) - 8, H / 2 - r), false, 0.5f, 0.0, false);
                this->text("3", Point(((W) / 2 - shiftLeft) + r - 8, H / 2 - 7), false, 0.5f, 0.0, false);
                this->text("6", Point(((W) / 2 - shiftLeft) - 3, H / 2 + r - 13), false, 0.5f, 0.0, false);
                this->text("9", Point(((W) / 2 - shiftLeft) - r + 4, H / 2 - 7), false, 0.5f, 0.0, false);
            }

            for (auto i = 0; i < 12; i++)
            {
                if constexpr (W > 32)
                    if (i % 3 == 0)
                        continue;
                this->drawLine(c[i * 5] + localCenterX, s[i * 5] + centerY, c[i * 5] * 0.8f + localCenterX, s[i * 5] * 0.8f + centerY, main_ticks);
            }

            int corrected_hour = hour * 5 + minute / 12; // additionally move hour hand a little bit every 12 minutes between hour marks
            this->drawLine(c[corrected_hour] * 0.1f + localCenterX + 1, s[corrected_hour] * 0.2f + centerY + 1, -c[corrected_hour] * 0.6f + localCenterX, -s[corrected_hour] * 0.6f + centerY, shadow_hand);
            this->drawLine(c[corrected_hour] * 0.1f + localCenterX, s[corrected_hour] * 0.2f + centerY, -c[corrected_hour] * 0.6f + localCenterX, -s[corrected_hour] * 0.6f + centerY, hour_hand);
            this->drawLine(c[minute] * 0.15f + localCenterX + 1, s[minute] * 0.2f + centerY + 1, -c[minute] * 0.85f + localCenterX, -s[minute] * 0.85f + centerY, shadow_hand);
            this->drawLine(c[minute] * 0.15f + localCenterX, s[minute] * 0.2f + centerY, -c[minute] * 0.85f + localCenterX, -s[minute] * 0.85f + centerY, minute_hand);
            this->drawLine(c[second] * 0.1f + localCenterX + 1, s[second] * 0.1f + centerY + 1, -c[second] * 0.85f + localCenterX, -s[second] * 0.85f + centerY, second_shadow_hand);
            this->drawLine(c[second] * 0.1f + localCenterX, s[second] * 0.1f + centerY, -c[second] * 0.85f + localCenterX, -s[second] * 0.85f + centerY, second_hand);
          
            if (image_ready) {

                // Draw the BMP on the right side of the screen
                for (int y = 0; y < IMG_H; ++y) {
                    for (int x = 0; x < IMG_W; ++x) {
                        size_t idx = (y * IMG_W + x) * 3;

                        uint8_t r = image_buf[idx];
                        uint8_t g = image_buf[idx + 1];
                        uint8_t b = image_buf[idx + 2];

                        this->set_pen(this->create_pen(r, g, b));
                        this->set_pixel(Point(OFFSET_X + x, OFFSET_Y + y));
                    }
                }

            }
            
        }

    }
};


int main()
{
    initialize();

    // Create analog clock using pico_graphics functionality
    static AnalogClock<Panel::SCREEN_WIDTH, Panel::SCREEN_HEIGHT> analogClock;

    // The Hub75 driver is constantly running on core 1 with a frequency usually much higher than 200Hz.
    // CPU load (on core 1) is low due to DMA and PIO usage.
    // The animated examples are updated at 100Hz.
    const float fps = 75.0f;
    const float frame_delay_ms = 1000.0f / fps;

    // set basis brightness of matrix panel
    driver.setBasisBrightness(8);

    // set full brightness of panel
    float intensity = 1.0f;
    driver.setIntensity(intensity);

    absolute_time_t next_frame = make_timeout_time_ms((uint32_t)frame_delay_ms);
    while (true)
    {
        analogClock.draw();
        driver.update(&analogClock);
        
 
        sleep_until(next_frame);
        next_frame = delayed_by_ms(next_frame, (uint32_t)frame_delay_ms);
    }
}