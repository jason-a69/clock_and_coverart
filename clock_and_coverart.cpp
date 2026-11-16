#include "Config.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"

#ifdef CYW43_WL_GPIO_LED_PIN
#include "pico/cyw43_arch.h"
#endif

#include "hub75.hpp"
#include "antialiased_line.hpp"

#include "pico/aon_timer.h"

using namespace pimoroni;
static PicoGraphics_PenRGB888 graphics(96, 48, nullptr);

#ifndef M_PI
#define M_PI (3.14159265358979323846)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/apps/sntp.h"
#include "pico/util/datetime.h"
#include "pico/aon_timer.h"
#include "pico/mutex.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include <cstring>
#include <array>
#include <time.h>

#ifndef WEATHER_LAT
  #ifdef SECRET_WEATHER_LAT
    #define WEATHER_LAT SECRET_WEATHER_LAT
  #else
    #define WEATHER_LAT ""
  #endif
#endif

#ifndef WEATHER_LON
  #ifdef SECRET_WEATHER_LON
    #define WEATHER_LON SECRET_WEATHER_LON
  #else
    #define WEATHER_LON ""
  #endif
#endif

#ifndef BED_TIME
  #ifdef SECRET_BED_TIME
    #define BED_TIME SECRET_BED_TIME
  #else
    #define BED_TIME ""
  #endif
#endif

#ifndef CARTOONS
  #ifdef SECRET_CARTOONS
    #define CARTOONS SECRET_CARTOONS
  #else
    #define CARTOONS "Yes"
  #endif
#endif
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

auto_init_mutex(aon_timer_mutex);
static bool aon_timer_is_initialised = false;

#define TCP_PORT 5000
#define IMG_SIZE (96 * 48 * 3)

#define CHECK_INTERVAL_US 300000000ULL
#define WEATHER_INTERVAL_US 900000000ULL

static std::array<uint8_t, IMG_SIZE> image_buf_arr;
static std::array<uint8_t, IMG_SIZE> current_image_buf_arr;
static uint8_t *image_buf = image_buf_arr.data();
static uint8_t *current_image_buf = current_image_buf_arr.data();

static size_t  recv_len = 0;
static bool    image_ready = false;
static bool    is_all_zero = true;

// ============================================================================
// WEATHER DATA & HTTP CLIENT (Open-Meteo API over port 80)
// ============================================================================
struct WeatherSlot {
    int hour;
    int temp_c;
    int rain_prob;
    int wmo_code;
};

static std::array<WeatherSlot, 4> forecast_12h = {{
    {0, 0, 0, 0},
    {3, 0, 0, 0},
    {6, 0, 0, 0},
    {9, 0, 0, 0}
}};

struct DailyWeatherSlot {
    int year;
    int month;
    int day;
    int temp_min;
    int temp_max;
    int rain_prob;
    int wmo_code;
};

static std::array<DailyWeatherSlot, 4> forecast_4d = {{}};

static bool weather_valid = false;
static bool daily_weather_valid = false;
static bool show_daily_forecast = false;
static bool snoopy_bedtime_active = false;
static int last_snoopy_day = -1;
static bool weather_just_updated = false;
static bool weather_fetch_in_progress = false;
static std::array<char, 4096> http_rx_buf;
static size_t http_rx_len = 0;

struct TCP_SERVER_T {
    struct tcp_pcb *server_pcb;
    struct tcp_pcb *client_pcb;
};

constexpr int PANEL_W = 96;
constexpr int PANEL_H = 48;

static std::array<uint8_t, PANEL_W * PANEL_H * 3> screen_buf;

constexpr Hub75Config panel_cfg{
    .panel = {
        .matrix_panel_width = PANEL_W,
        .matrix_panel_height = PANEL_H,
        .chain_rows = 1,
        .chain_cols = 1,
        .chain_mode = Hub75ChainMode::SERPENTINE,
        .panel_kind = RowMapping::Standard,
        .address_type = RowAddressing::ABCShiftRegister,
        .scan_mode = 24,
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
static Panel driver;
// ============================================================================
// OPEN-METEO JSON PARSER & LWIP HTTP CLIENT
// ============================================================================
static const char* find_json_array(const char *root, const char *key) {
    const char *k = strstr(root, key);
    if (!k) return nullptr;
    const char *bracket = strchr(k + strlen(key), '[');
    return bracket ? (bracket + 1) : nullptr;
}

static const char* day_of_week_str(int y, int m, int d) {
    static const std::array<int, 12> t = {{0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4}};
    static const std::array<const char*, 7> names = {{
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    }};
    if (m < 3) y -= 1;
    int dow = (y + y / 4 - y / 100 + y / 400 + t.at(m - 1) + d) % 7;
    if (dow < 0 || dow > 6) return "Day";
    return names.at(dow);
}

static void parse_open_meteo_json(const char *json) {
    const char *hourly = strstr(json, "\"hourly\"");
    if (!hourly) {
        printf("Weather JSON error: missing 'hourly' object\n");
        return;
    }

    const char *p_time = find_json_array(hourly, "\"time\"");
    const char *p_temp = find_json_array(hourly, "\"temperature_2m\"");
    const char *p_prob = find_json_array(hourly, "\"precipitation_probability\"");
    const char *p_code = find_json_array(hourly, "\"weather_code\"");
    if (!p_code) p_code = find_json_array(hourly, "\"weathercode\"");

    if (!p_time || !p_temp || !p_prob || !p_code) {
        printf("Weather JSON error: missing expected arrays\n");
        return;
    }

    std::array<int, 12> hours = {};
    std::array<float, 12> temps = {};
    std::array<int, 12> probs = {};
    std::array<int, 12> codes = {};

    const char *cur = p_time;
    int count = 0;
    while (cur && *cur && *cur != ']' && count < 12) {
        const char *q1 = strchr(cur, '"');
        if (!q1 || q1 > strchr(cur, ']')) break;
        const char *t_mark = strchr(q1, 'T');
        const char *q2 = strchr(q1 + 1, '"');
        if (!t_mark || !q2 || t_mark > q2) break;
        hours.at(count++) = atoi(t_mark + 1);
        cur = q2 + 1;
    }

    cur = p_temp;
    for (int i = 0; i < count && cur && *cur && *cur != ']'; i++) {
        while (*cur == ' ' || *cur == ',' || *cur == '\n' || *cur == '\r') cur++;
        if (*cur == ']') break;
        if (strncmp(cur, "null", 4) == 0) {
            temps.at(i) = 0.0f;
            cur += 4;
        } else {
            char *endp = nullptr;
            temps.at(i) = strtof(cur, &endp);
            if (endp == cur) break;
            cur = endp;
        }
    }

    cur = p_prob;
    for (int i = 0; i < count && cur && *cur && *cur != ']'; i++) {
        while (*cur == ' ' || *cur == ',' || *cur == '\n' || *cur == '\r') cur++;
        if (*cur == ']') break;
        if (strncmp(cur, "null", 4) == 0) {
            probs.at(i) = 0;
            cur += 4;
        } else {
            char *endp = nullptr;
            probs.at(i) = (int)strtol(cur, &endp, 10);
            if (endp == cur) break;
            cur = endp;
        }
    }

    cur = p_code;
    for (int i = 0; i < count && cur && *cur && *cur != ']'; i++) {
        while (*cur == ' ' || *cur == ',' || *cur == '\n' || *cur == '\r') cur++;
        if (*cur == ']') break;
        if (strncmp(cur, "null", 4) == 0) {
            codes.at(i) = 0;
            cur += 4;
        } else {
            char *endp = nullptr;
            codes.at(i) = (int)strtol(cur, &endp, 10);
            if (endp == cur) break;
            cur = endp;
        }
    }

    if (count < 1) {
        printf("Weather JSON error: parsed 0 entries\n");
        return;
    }

    for (int s = 0; s < 4; s++) {
        int idx = s * 3;
        if (idx >= count) idx = count - 1;
        forecast_12h.at(s).hour = hours.at(idx);
        forecast_12h.at(s).temp_c = (int)roundf(temps.at(idx));
        forecast_12h.at(s).rain_prob = probs.at(idx);
        forecast_12h.at(s).wmo_code = codes.at(idx);
    }
    const char *daily = strstr(json, "\"daily\"");
    if (daily) {
        const char *d_time = find_json_array(daily, "\"time\"");
        const char *d_tmax = find_json_array(daily, "\"temperature_2m_max\"");
        const char *d_tmin = find_json_array(daily, "\"temperature_2m_min\"");
        const char *d_prob = find_json_array(daily, "\"precipitation_probability_max\"");
        const char *d_code = find_json_array(daily, "\"weather_code\"");
        if (!d_code) d_code = find_json_array(daily, "\"weathercode\"");

        if (d_time && d_tmax && d_tmin && d_prob && d_code) {
            std::array<int, 5> d_years = {};
            std::array<int, 5> d_months = {};
            std::array<int, 5> d_days = {};
            std::array<float, 5> d_temps_max = {};
            std::array<float, 5> d_temps_min = {};
            std::array<int, 5> d_probs = {};
            std::array<int, 5> d_codes = {};

            const char *dcur = d_time;
            int dcount = 0;
            while (dcur && *dcur && *dcur != ']' && dcount < 5) {
                const char *q1 = strchr(dcur, '"');
                if (!q1 || q1 > strchr(dcur, ']')) break;
                const char *q2 = strchr(q1 + 1, '"');
                if (!q2) break;
                if (q2 - q1 >= 11) {
                    d_years.at(dcount) = atoi(q1 + 1);
                    d_months.at(dcount) = atoi(q1 + 6);
                    d_days.at(dcount) = atoi(q1 + 9);
                    dcount++;
                }
                dcur = q2 + 1;
            }

            dcur = d_tmax;
            for (int i = 0; i < dcount && dcur && *dcur && *dcur != ']'; i++) {
                while (*dcur == ' ' || *dcur == ',' || *dcur == '\n' || *dcur == '\r') dcur++;
                if (*dcur == ']') break;
                if (strncmp(dcur, "null", 4) == 0) {
                    d_temps_max.at(i) = 0.0f;
                    dcur += 4;
                } else {
                    char *endp = nullptr;
                    d_temps_max.at(i) = strtof(dcur, &endp);
                    if (endp == dcur) break;
                    dcur = endp;
                }
            }

            dcur = d_tmin;
            for (int i = 0; i < dcount && dcur && *dcur && *dcur != ']'; i++) {
                while (*dcur == ' ' || *dcur == ',' || *dcur == '\n' || *dcur == '\r') dcur++;
                if (*dcur == ']') break;
                if (strncmp(dcur, "null", 4) == 0) {
                    d_temps_min.at(i) = 0.0f;
                    dcur += 4;
                } else {
                    char *endp = nullptr;
                    d_temps_min.at(i) = strtof(dcur, &endp);
                    if (endp == dcur) break;
                    dcur = endp;
                }
            }

            dcur = d_prob;
            for (int i = 0; i < dcount && dcur && *dcur && *dcur != ']'; i++) {
                while (*dcur == ' ' || *dcur == ',' || *dcur == '\n' || *dcur == '\r') dcur++;
                if (*dcur == ']') break;
                if (strncmp(dcur, "null", 4) == 0) {
                    d_probs.at(i) = 0;
                    dcur += 4;
                } else {
                    char *endp = nullptr;
                    d_probs.at(i) = (int)strtol(dcur, &endp, 10);
                    if (endp == dcur) break;
                    dcur = endp;
                }
            }

            dcur = d_code;
            for (int i = 0; i < dcount && dcur && *dcur && *dcur != ']'; i++) {
                while (*dcur == ' ' || *dcur == ',' || *dcur == '\n' || *dcur == '\r') dcur++;
                if (*dcur == ']') break;
                if (strncmp(dcur, "null", 4) == 0) {
                    d_codes.at(i) = 0;
                    dcur += 4;
                } else {
                    char *endp = nullptr;
                    d_codes.at(i) = (int)strtol(dcur, &endp, 10);
                    if (endp == dcur) break;
                    dcur = endp;
                }
            }

            if (dcount >= 5) {
                for (int s = 0; s < 4; s++) {
                    int idx = s + 1;
                    forecast_4d.at(s).year = d_years.at(idx);
                    forecast_4d.at(s).month = d_months.at(idx);
                    forecast_4d.at(s).day = d_days.at(idx);
                    forecast_4d.at(s).temp_min = (int)roundf(d_temps_min.at(idx));
                    forecast_4d.at(s).temp_max = (int)roundf(d_temps_max.at(idx));
                    forecast_4d.at(s).rain_prob = d_probs.at(idx);
                    forecast_4d.at(s).wmo_code = d_codes.at(idx);
                }
                daily_weather_valid = true;
            }
        }
    }

    weather_valid = true;
    weather_just_updated = true;
    printf("Weather updated (%d hrs, daily=%d)\n", count, daily_weather_valid ? 1 : 0);
}
static err_t weather_http_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (!p) {
        http_rx_buf.at(http_rx_len) = '\0';
        const char *line_end = strstr(http_rx_buf.data(), "\r\n");
        if (line_end) {
            printf("Weather HTTP status: %.*s\n", (int)(line_end - http_rx_buf.data()), http_rx_buf.data());
        }
        const char *body = strstr(http_rx_buf.data(), "\r\n\r\n");
        if (body) {
            body += 4;
            const char *json_start = strchr(body, '{');
            if (json_start) {
                parse_open_meteo_json(json_start);
            } else {
                printf("Weather HTTP body missing '{': %.120s\n", body);
            }
        }
        tcp_close(tpcb);
        weather_fetch_in_progress = false;
        return ERR_OK;
    }

    size_t copy_len = p->tot_len;
    if (http_rx_len + copy_len >= http_rx_buf.size() - 1) {
        copy_len = http_rx_buf.size() - 1 - http_rx_len;
    }
    if (copy_len > 0) {
        pbuf_copy_partial(p, http_rx_buf.data() + http_rx_len, copy_len, 0);
        http_rx_len += copy_len;
    }
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void weather_http_err(void *arg, err_t err) {
    printf("Weather HTTP error: %d\n", err);
    weather_fetch_in_progress = false;
}

static void clean_coord_str(const char *raw, char *out, size_t out_sz) {
    size_t j = 0;
    for (size_t i = 0; raw[i] != '\0' && j + 1 < out_sz; i++) {
        if (raw[i] != '"') {
            out[j++] = raw[i];
        }
    }
    out[j] = '\0';
}

static bool has_weather_location() {
    std::array<char, 24> lat_clean;
    std::array<char, 24> lon_clean;
    clean_coord_str(STR(WEATHER_LAT), lat_clean.data(), lat_clean.size());
    clean_coord_str(STR(WEATHER_LON), lon_clean.data(), lon_clean.size());
    return (lat_clean.at(0) != '\0' && lon_clean.at(0) != '\0');
}

static bool parse_bed_time(int &out_hour, int &out_min) {
    std::array<char, 16> raw_clean;
    clean_coord_str(STR(BED_TIME), raw_clean.data(), raw_clean.size());
    if (raw_clean.at(0) == '\0') {
        return false;
    }
    const char *colon = strchr(raw_clean.data(), ':');
    if (!colon) {
        return false;
    }
    int h = atoi(raw_clean.data());
    int m = atoi(colon + 1);
    if (h < 0 || h > 23 || m < 0 || m > 59) {
        return false;
    }
    out_hour = h;
    out_min = m;
    return true;
}

static bool is_cartoons_enabled() {
    std::array<char, 16> raw_clean;
    clean_coord_str(STR(CARTOONS), raw_clean.data(), raw_clean.size());
    return (raw_clean.at(0) == 'Y' || raw_clean.at(0) == 'y');
}

static err_t weather_http_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    if (err != ERR_OK) {
        weather_fetch_in_progress = false;
        return err;
    }

    std::array<char, 24> lat_clean;
    std::array<char, 24> lon_clean;
    clean_coord_str(STR(WEATHER_LAT), lat_clean.data(), lat_clean.size());
    clean_coord_str(STR(WEATHER_LON), lon_clean.data(), lon_clean.size());

    std::array<char, 512> req;
    snprintf(req.data(), req.size(),
             "GET /v1/forecast?latitude=%s&longitude=%s"
             "&hourly=temperature_2m,precipitation_probability,weather_code"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&forecast_hours=12&forecast_days=5&timezone=auto HTTP/1.0\r\n"
             "Host: api." "open-meteo.com\r\n"
             "User-Agent: PicoW-WeatherClock/1.0\r\n"
             "Accept: application/json\r\n"
             "Accept-Encoding: identity\r\n"
             "Connection: close\r\n\r\n",
             lat_clean.data(), lon_clean.data());

    tcp_recv(tpcb, weather_http_recv);
    tcp_write(tpcb, req.data(), strlen(req.data()), TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
    return ERR_OK;
}

static void weather_dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg) {
    if (!ipaddr) {
        printf("DNS failed for %s\n", name);
        weather_fetch_in_progress = false;
        return;
    }

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) {
        weather_fetch_in_progress = false;
        return;
    }

    http_rx_len = 0;
    tcp_err(pcb, weather_http_err);
    err_t err = tcp_connect(pcb, ipaddr, 80, weather_http_connected);
    if (err != ERR_OK) {
        tcp_abort(pcb);
        weather_fetch_in_progress = false;
    }
}

void fetch_weather() {
    if (!has_weather_location()) {
        return;
    }
    if (weather_fetch_in_progress) return;
    weather_fetch_in_progress = true;

    ip_addr_t target_ip;
    cyw43_arch_lwip_begin();
    err_t err = dns_gethostbyname("api." "open-meteo.com", &target_ip, weather_dns_cb, nullptr);
    if (err == ERR_OK) {
        weather_dns_cb("api." "open-meteo.com", &target_ip, nullptr);
    } else if (err != ERR_INPROGRESS) {
        weather_fetch_in_progress = false;
    }
    cyw43_arch_lwip_end();
}
// ============================================================================
// LED & CORE1 INITIALIZATION
// ============================================================================
int pico_led_init(void)
{
#if defined(PICO_DEFAULT_LED_PIN)
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    return PICO_OK;
#elif defined(CYW43_WL_GPIO_LED_PIN)
    return cyw43_arch_init();
#else
    return PICO_OK;
#endif
}

void pico_set_led(bool led_on)
{
#if defined(PICO_DEFAULT_LED_PIN)
    gpio_put(PICO_DEFAULT_LED_PIN, led_on);
#elif defined(CYW43_WL_GPIO_LED_PIN)
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on);
#endif
}

int led_init(void)
{
    int rc = pico_led_init();
    hard_assert(rc == PICO_OK);

    for (int i = 0; i < 8; i++)
    {
        pico_set_led(true);
        sleep_ms(250);
        pico_set_led(false);
        sleep_ms(250);
    }
    return PICO_OK;
}

void core1_entry()
{
    driver.create();
    driver.start();

    while (true)
    {
        tight_loop_contents();
    }
}

void sntp_set_system_time_us(uint32_t sec, uint32_t us) {    
    static struct timespec ntp_ts;
    
    printf("Updating time to match timezone %s\n", TZ);
    setenv("TZ", TZ, 1);
    tzset();
    
    int offset_seconds = (int)(-_timezone);
    printf("Offset seconds from the timezone is %d\n", offset_seconds);

    ntp_ts.tv_sec = sec + offset_seconds;
    ntp_ts.tv_nsec = us * 1000;

    if (aon_timer_is_initialised) {
        if (mutex_enter_timeout_ms(&aon_timer_mutex, 10)) {
            aon_timer_set_time(&ntp_ts);
            mutex_exit(&aon_timer_mutex);
            puts("-> Updated system time from NTP");
        } else {
            puts("-> Skipped NTP system time update (aon_timer was busy)");
        }
    } else {
        aon_timer_is_initialised = aon_timer_start(&ntp_ts);
        puts("-> initialised system time from NTP");
    }
}

void sntp_get_system_time_us(uint32_t *sec_ptr, uint32_t * us_ptr) {
    static struct timespec sys_ts;
    aon_timer_get_time(&sys_ts);
    *sec_ptr = sys_ts.tv_sec;
    *us_ptr = sys_ts.tv_nsec / 1000;
}

// ============================================================================
// TCP SERVER FOR ROON COVER ART
// ============================================================================
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (!p) {
        printf("In tcp_server_recv - connection closed\n");
        tcp_close(tpcb);
        return ERR_OK;
    }

    size_t copy_len = p->tot_len;
    if (recv_len + copy_len > IMG_SIZE + 8)
        copy_len = IMG_SIZE + 8 - recv_len;

    pbuf_copy_partial(p, image_buf + recv_len, copy_len, 0);
    recv_len += copy_len;
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    if (recv_len >= 8) {
        if (memcmp(image_buf, "BMP1", 4) == 0) {
            uint32_t expected;
            memcpy(&expected, image_buf + 4, 4);
            if (recv_len >= 8 + expected) {
                uint32_t pixel_data_size = expected;
                memmove(image_buf, image_buf + 8, expected);
                printf("Received an image\n");

                bool was_zero = is_all_zero;
                is_all_zero = true;
                for (uint32_t i = 0; i < pixel_data_size; i++) {
                    if (image_buf[i] != 0) {
                        is_all_zero = false;
                        break;
                    }
                }

                if (is_all_zero) {
                    printf("Verification: Image data is confirmed all zeroes (Black) -> Showing Weather\n");
                    if (!was_zero || !weather_valid) {
                        fetch_weather();
                    }
                    weather_just_updated = true;
                } else {
                    printf("Verification: Image contains active pixel colors -> Showing Cover Art\n");
                }

                image_ready = true;
                recv_len = 0;
            }
        }
    }
    return ERR_OK;
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    if (err != ERR_OK || client_pcb == NULL) {
        printf("Returning, error in tcp_server_accept %d\n", err);
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
        printf("tcp bind error in tcp_server_open %d\n", err);
        return false;
    }
     
    struct tcp_pcb *server = tcp_listen_with_backlog(pcb, 1);
    if (!server) {
        printf("Server error in struct tcp_pcb\n");
        return false;
    }

    tcp_accept(server, tcp_server_accept);
    return true;
}

void connect_wifi()
{
    uint32_t connection_timeout_ms = 30000;

    printf("Connecting to Wi-Fi...\n");
    printf("Wifi SSID is %s\n", WIFI_SSID);
    int status = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, connection_timeout_ms);
    if (status != 0) {
        printf("Wifi error in connecting is %d\n", status);
    }

    ip4_addr_t ip, netmask, gw;
    if (!ip4addr_aton(STATIC_IP, &ip) ||
        !ip4addr_aton(NETMASK, &netmask) ||
        !ip4addr_aton(GATEWAY, &gw)) {
        printf("Invalid IP configuration in cmake.secret\n");
    }

    cyw43_arch_lwip_begin();
    netif_set_addr(netif_default, &ip, &netmask, &gw);
    cyw43_arch_lwip_end();

    printf("Connected with static IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
}

void initialize()
{
    stdio_init_all();
    led_init();

    multicore_reset_core1();
    multicore_launch_core1(core1_entry);

    cyw43_arch_enable_sta_mode();
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);

    connect_wifi();

    if (!tcp_server_open()) {
        printf("TCP server failed\n");
    } else {
        printf("Listening on port %d\n", TCP_PORT);
    }

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_init();
}
// ============================================================================
// DISPLAY CLASS: ANALOG CLOCK (LEFT) + COVER ART OR WEATHER (RIGHT)
// ============================================================================
template <uint32_t W, uint32_t H>
class AnalogClock : public AntialiasedLine<W, H>
{
private:
    float r = 1.0f;
    uint l = 1;
    float da = M_PI / 30.0f;
    float centerX = 0.0f;
    float centerY = 0.0f;

    std::array<float, 60> s;
    std::array<float, 60> c;

    int hour = 0;
    int minute = 0;
    int second = 0;
    int previous_second = second;

    constexpr static uint32_t hour_hand = 0x1bb1e7;
    constexpr static int32_t minute_hand = 0x11819d;
    constexpr static uint32_t second_hand = 0xF3D459;
    constexpr static uint32_t second_shadow_hand = 0x7a6203;
    constexpr static uint32_t shadow_hand = 0xffffff;
    constexpr static uint32_t main_ticks = 0xffffff;
    constexpr static uint32_t clock_digits = 0x7A263A;

    bool is_transitioning = false;
    uint32_t transition_step = 0;
    uint32_t active_transition = 0;
    const uint32_t TOTAL_STEPS = 32;

    uint32_t weather_flair_frame = 0;
    const uint32_t MAX_FLAIR_FRAMES = 360;
    int active_character = 0;

    uint32_t hash_coor(int x, int y) {
        uint32_t hash = y * 1327 + x * 4349;
        hash = (hash ^ 61) ^ (hash >> 16);
        hash += hash << 3;
        hash ^= hash >> 4;
        hash *= 0x27d4eb2d;
        hash ^= hash >> 15;
        return hash;
    }

    void draw_tiny_char(int x, int y, char ch, uint32_t color) {
        static const std::array<std::array<uint8_t, 3>, 34> glyphs = {{
            {{0x1F, 0x11, 0x1F}}, // 0
            {{0x00, 0x1F, 0x00}}, // 1
            {{0x1D, 0x15, 0x17}}, // 2
            {{0x15, 0x15, 0x1F}}, // 3
            {{0x07, 0x04, 0x1F}}, // 4
            {{0x17, 0x15, 0x1D}}, // 5
            {{0x1F, 0x15, 0x1D}}, // 6
            {{0x01, 0x01, 0x1F}}, // 7
            {{0x1F, 0x15, 0x1F}}, // 8
            {{0x17, 0x15, 0x1F}}, // 9
            {{0x04, 0x04, 0x04}}, // 10: -
            {{0x13, 0x08, 0x19}}, // 11: %
            {{0x03, 0x03, 0x00}}, // 12: *
            {{0x1F, 0x11, 0x11}}, // 13: C
            {{0x1F, 0x02, 0x1F}}, // 14: N
            {{0x1C, 0x14, 0x1C}}, // 15: o
            {{0x1F, 0x08, 0x1F}}, // 16: w
            {{0x04, 0x0E, 0x04}}, // 17: +
            {{0x1F, 0x04, 0x1C}}, // 18: h
            {{0x10, 0x00, 0x00}}, // 19: .
            {{0x18, 0x04, 0x03}}, // 20: /
            {{0x1F, 0x06, 0x1F}}, // 21: M
            {{0x01, 0x1F, 0x01}}, // 22: T
            {{0x1F, 0x0C, 0x1F}}, // 23: W
            {{0x1F, 0x05, 0x01}}, // 24: F
            {{0x17, 0x15, 0x1D}}, // 25: S
            {{0x1C, 0x10, 0x1C}}, // 26: u
            {{0x1C, 0x04, 0x18}}, // 27: n
            {{0x1C, 0x14, 0x16}}, // 28: e
            {{0x18, 0x14, 0x1F}}, // 29: d
            {{0x1C, 0x08, 0x04}}, // 30: r
            {{0x00, 0x1D, 0x00}}, // 31: i
            {{0x18, 0x14, 0x1C}}, // 32: a
            {{0x04, 0x1F, 0x14}}, // 33: t
        }};

        int idx = -1;
        if (ch >= '0' && ch <= '9') idx = ch - '0';
        else if (ch == '-') idx = 10;
        else if (ch == '%') idx = 11;
        else if (ch == '*') idx = 12;
        else if (ch == 'C') idx = 13;
        else if (ch == 'N') idx = 14;
        else if (ch == 'o') idx = 15;
        else if (ch == 'w') idx = 16;
        else if (ch == '+') idx = 17;
        else if (ch == 'h') idx = 18;
        else if (ch == '.') idx = 19;
        else if (ch == '/') idx = 20;
        else if (ch == 'M') idx = 21;
        else if (ch == 'T') idx = 22;
        else if (ch == 'W') idx = 23;
        else if (ch == 'F') idx = 24;
        else if (ch == 'S') idx = 25;
        else if (ch == 'u') idx = 26;
        else if (ch == 'n') idx = 27;
        else if (ch == 'e') idx = 28;
        else if (ch == 'd') idx = 29;
        else if (ch == 'r') idx = 30;
        else if (ch == 'i') idx = 31;
        else if (ch == 'a') idx = 32;
        else if (ch == 't') idx = 33;
        if (idx < 0) return;

        this->set_pen(color);
        for (int col = 0; col < 3; col++) {
            uint8_t bits = glyphs.at(idx).at(col);
            for (int row = 0; row < 5; row++) {
                if (bits & (1 << row)) {
                    int px = x + col;
                    int py = y + row;
                    if (px >= (PANEL_W / 2) && px < PANEL_W && py >= 0 && py < PANEL_H) {
                        this->set_pixel(Point(px, py));
                    }
                }
            }
        }
    }

    void draw_tiny_str(int x, int y, const char *str, uint32_t color) {
        while (*str) {
            draw_tiny_char(x, y, *str, color);
            x += 4;
            str++;
        }
    }
    void draw_character(int char_id, int px_left, int py_top, int step_frame, bool waving) {
        static constexpr std::array<const char*, 672> sprites = {{
            // 0: Pikachu (Pull)
            "....K.....K..", "....KY...YK..", "....YY...YY..", ".BB..YYYYYYY.",
            "BB..YYYYKYYKY", "BB..YYYYYYYRY", ".BB.YYYYYYYY.", "..BBYYYYYYY..",
            "...YYYYYYYYY.", "..YYYYYYYYY..", "..YYYYYYYY...", "...YYYYYYY...",
            "...YY...YY...", "..KK.....KK..",
            // 0: Pikachu (Wave)
            "..K.......K..", "..KY.....YK..", "..YY.....YY..", "...YYYYYYY...",
            "..YYKYYYKYY..", "..YRYYYYYRY..", "...YYYYYYY.YY", "..YYYYYYYYYY.",
            ".YYYYYYYYYY..", "..YYYYYYYY...", "..YYYYYYYY...", "...YYYYYY....",
            "...YY..YY....", "..KK....KK...",

            // 1: Hello Kitty (Pull)
            "...W.....W...", "..WWW...WMM..", "..WWWWWWMRM..", "..WWWWWWWMM..",
            "..WKWWWKWWW..", "..WWWYWWWWW..", "..WWWWWWWWW..", "....RRRRR....",
            "...UUUUUUU...", "..W.UUUUU.W..", "....UUUUU....", "....WW.WW....",
            "....WW.WW....", "...WWW.WWW...",
            // 1: Hello Kitty (Wave)
            "...W.....W...", "..WWW...WMM..", "..WWWWWWMRM..", "..WWWWWWWMM..",
            "..WKWWWKWWW..", "..WWWYWWWWW..", "..WWWWWWWWW..", "....RRRRR.WW.",
            "...UUUUUUWW..", "..W.UUUUU....", "....UUUUU....", "....WW.WW....",
            "....WW.WW....", "...WWW.WWW...",

            // 2: Mickey Mouse (Pull)
            "..KK.....KK..", ".KKKK...KKKK.", "..KK.KKK.KK..", "....KKKKK....",
            "....KPPPK....", "....KPKPK....", ".....PPP.....", "...KKKKKKK...",
            "..W.RRRRR.W..", "....RWWRW....", "....RRRRR....", "....KK.KK....",
            "...YY...YY...", "..YYY...YYY..",
            // 2: Mickey Mouse (Wave)
            "..KK.....KK..", ".KKKK...KKKK.", "..KK.KKK.KK..", "....KKKKK....",
            "....KPPPK....", "....KPKPK....", ".....PPP..WW.", "...KKKKKKKWW.",
            "..W.RRRRRK...", "....RWWRW....", "....RRRRR....", "....KK.KK....",
            "...YY...YY...", "..YYY...YYY..",

            // 3: Minnie Mouse (Pull)
            "..KK.MMM.KK..", ".KKKKMRMKKKK.", "..KK.MMM.KK..", "....KKKKK....",
            "....KPPPK....", "....KPKPK....", ".....PPP.....", "...KKKKKKK...",
            "..W.MMMMM.W..", "...MWMWMWM...", "...MMMMMMM...", "....KK.KK....",
            "...YY...YY...", "..YYY...YYY..",
            // 3: Minnie Mouse (Wave)
            "..KK.MMM.KK..", ".KKKKMRMKKKK.", "..KK.MMM.KK..", "....KKKKK....",
            "....KPPPK....", "....KPKPK....", ".....PPP..WW.", "...KKKKKKKWW.",
            "..W.MMMMMK...", "...MWMWMWM...", "...MMMMMMM...", "....KK.KK....",
            "...YY...YY...", "..YYY...YYY..",

            // 4: Donald Duck (Pull)
            "....UUUUU....", "...UUUUUUU...", "..KKKKKKKKK..", "....WWWWW....",
            "....WKWKW....", "....WWWWOOO..", ".....WOOOO...", "....UUUUU....",
            "...UUURUUU...", "..W.UUUUU.W..", "....WWWWW....", "....WW.WW....",
            "....OO.OO....", "...OOO.OOO...",
            // 4: Donald Duck (Wave)
            "....UUUUU....", "...UUUUUUU...", "..KKKKKKKKK..", "....WWWWW....",
            "....WKWKW....", ".....WOOO....", "....WOOOO.WW.", "....UUUUUWW..",
            "...UUURUUU...", "..W.UUUUU....", "....WWWWW....", "....WW.WW....",
            "....OO.OO....", "...OOO.OOO...",

            // 5: Goofy (Pull)
            ".....EEE.....", "....EEEEE....", "...EEEEEEE...", "..KKPPPPPKK..",
            ".KK.PKPPK.KK.", ".KK.PPPPP.KK.", ".....WWW.....", "....OOOOO....",
            "..P.OOOOO.P..", "....UUUUU....", "....UU.UU....", "....UU.UU....",
            "...BB...BB...", "..BBB...BBB..",
            // 5: Goofy (Wave)
            ".....EEE.....", "....EEEEE....", "...EEEEEEE...", "..KKPPPPPKK..",
            ".KK.PKPPK.KK.", ".KK.PPPPP.KK.", ".....WWW..PP.", "....OOOOOPP..",
            "..P.OOOOO....", "....UUUUU....", "....UU.UU....", "....UU.UU....",
            "...BB...BB...", "..BBB...BBB..",
            // 6: Pluto (Pull)
            "....YYYYY....", "...YYYYYYY...", "..KKYYYYYKK..", ".KK.YKYYK.KK.",
            ".KK.YYYYYKK..", ".....YYY.....", "....EEEEE....", "...YYYYYYY...",
            "..YYYYYYYYY..", "...YYYYYYY...", "....YYYYY....", "....YY.YY....",
            "....YY.YY....", "...YYY.YYY...",
            // 6: Pluto (Wave)
            "....YYYYY....", "...YYYYYYY...", "..KKYYYYYKK..", ".KK.YKYYK.KK.",
            ".KK.YYYYYKK..", ".....YYY.....", "....EEEEE.YY.", "...YYYYYYYY..",
            "..YYYYYYYY...", "...YYYYYY....", "....YY.YY....", "....YY.YY....",
            "....YY.YY....", "...YYY.YYY...",

            // 7: Winnie the Pooh (Pull)
            "...YY...YY...", "..YYYYYYYYY..", "..YYKYYYKYY..", "..YYYYKYYYY..",
            "...YYYYYYY...", "....RRRRR....", "...RRRRRRR...", "..YRRRRRRRY..",
            "...RRRRRRR...", "...YYYYYYY...", "....YY.YY....", "....YY.YY....",
            "...YY...YY...", "..YYY...YYY..",
            // 7: Winnie the Pooh (Wave)
            "...YY...YY...", "..YYYYYYYYY..", "..YYKYYYKYY..", "..YYYYKYYYY..",
            "...YYYYYYY...", "....RRRRR.YY.", "...RRRRRRYY..", "..YRRRRRR....",
            "...RRRRRRR...", "...YYYYYYY...", "....YY.YY....", "....YY.YY....",
            "...YY...YY...", "..YYY...YYY..",

            // 8: Stitch (Pull)
            ".CC.......CC.", "CMCC.CCC.CCMC", "CMCCCCCCCCCMP", ".CC.CCKCCKCC.",
            "....CCCCKCC..", ".....CCCCC...", "....CCCCCCC..", "...CCCCUCCCC.",
            "..C.CCCUCCC.C", "....CCCCCCC..", "....CC...CC..", "....CC...CC..",
            "...CC.....CC.", "..CCC.....CCC",
            // 8: Stitch (Wave)
            ".CC.......CC.", "CMCC.CCC.CCMC", "CMCCCCCCCCCMP", ".CC.CCKCCKCC.",
            "....CCCCKCC..", ".....CCCCC...", "....CCCCCC.CC", "...CCCCUCCCC.",
            "..C.CCCUCCC..", "....CCCCCCC..", "....CC...CC..", "....CC...CC..",
            "...CC.....CC.", "..CCC.....CCC",

            // 9: Snoopy (Pull)
            ".....WWWW....", "....WWWWWW...", "...KKWWWWWK..", "..KKKWWWWWW..",
            "..KKKWWWWWW..", "...KK.WWWW...", ".....RRRRR...", "....WWWWWWW..",
            "...WWWWWWWWW.", "...WWWWWWWWW.", "....WWWWWWW..", "....WW...WW..",
            "...WW.....WW.", "..KKK.....KKK",
            // 9: Snoopy (Wave)
            "....WWWW.....", "...WWWWWW....", "..KKWWWWWK...", ".KKKWWWWWW...",
            ".KKKWWWWWW...", "..KK.WWWW....", "....RRRRR.WW.", "...WWWWWWWW..",
            "..WWWWWWWW...", "...WWWWWW....", "....WW.WW....", "....WW.WW....",
            "...WW...WW...", "..KKK...KKK..",

            // 10: Charlie Brown (Pull)
            "....PPPPP....", "...PPPPPPP...", "...PPKPPKP...", "...PPPPPPP...",
            "....PPKPP....", ".....PPP.....", "....YYYYY....", "...YYYYYYY...",
            "..P.YKYKY.P..", "....YYYYY....", "....KKKKK....", "....PP.PP....",
            "...BB...BB...", "..BBB...BBB..",
            // 10: Charlie Brown (Wave)
            "....PPPPP....", "...PPPPPPP...", "...PPKPPKP...", "...PPPPPPP...",
            "....PPKPP....", ".....PPP..PP.", "....YYYYYPP..", "...YYYYYYY...",
            "..P.YKYKY....", "....YYYYY....", "....KKKKK....", "....PP.PP....",
            "...BB...BB...", "..BBB...BBB..",

            // 11: Woodstock (Pull)
            "...Y.Y.Y.....", "....YYYYY....", "...YYYYYY....", "....YYKYYO...",
            ".....YYOOO...", "......YYY....", ".....YYYYY...", "....YYYYYYY..",
            "...Y.YYYYY.Y.", ".....YYYYY...", "......YYY....", ".....O...O...",
            "....OO...OO..", "...OOO...OOO.",
            // 11: Woodstock (Wave)
            "...Y.Y.Y.....", "....YYYYY....", "...YYYYYY....", "....YYKYYO...",
            ".....YYOOO...", "......YYY..YY", ".....YYYYYYY.", "....YYYYYY...",
            "...Y.YYYYY...", ".....YYYYY...", "......YYY....", ".....O...O...",
            "....OO...OO..", "...OOO...OOO.",
            // 12: Asterix (Pull)
            ".WW.......WW.", "WWWW.GGG.WWWW", ".WW.GGGGG.WW.", "....PPPPP....",
            "....PKPPK....", "....PPYYYP...", ".....YYYY....", "....KKKKK....",
            "...KKKKKKK...", "..P.KYYYK.P..", "....RRRRR....", "....RR.RR....",
            "...BB...BB...", "..BBB...BBB..",
            // 12: Asterix (Wave)
            ".WW.......WW.", "WWWW.GGG.WWWW", ".WW.GGGGG.WW.", "....PPPPP....",
            "....PKPPK....", "....PYYYP....", ".....YYYY.PP.", "....KKKKKPP..",
            "...KKKKKKK...", "..P.KYYYK....", "....RRRRR....", "....RR.RR....",
            "...BB...BB...", "..BBB...BBB..",

            // 13: Obelix (Pull)
            ".W.........W.", "WW..GGGGG..WW", ".R.GGGGGGG.R.", "RR..PPPPP..RR",
            "RR..PKPPK..RR", "R...PPRRP...R", "....PPPPPP...", "..PPPPPPPPPP.",
            ".PPPPPPPPPPPP", "P.WWWWWWWWWWP", "..UUUUUUUUUU.", "..WWWW..WWWW.",
            "..UUUU..UUUU.", ".BBBB....BBBB",
            // 13: Obelix (Wave)
            ".W.........W.", "WW..GGGGG..WW", ".R.GGGGGGG.R.", "RR..PPPPP..RR",
            "RR..PKPPK..RR", "R...PPRRP...R", "....PPPPPP.PP", "..PPPPPPPPPP.",
            ".PPPPPPPPPP..", "P.WWWWWWWW...", "..UUUUUUUU...", "..WWWW..WWWW.",
            "..UUUU..UUUU.", ".BBBB....BBBB",

            // 14: Tom (Pull)
            "...G.....G...", "..GG.....GG..", "..GGGGGGGGG..", "..GYYGYYGGG..",
            "..GGWWKWWGG..", "...GWWWWW....", "....GGWGG....", "...GGGGGGG...",
            "..G.GGWGG.G..", "...GGGGGGG...", "....GG.GG....", "....GG.GG....",
            "...WW...WW...", "..WWW...WWW..",
            // 14: Tom (Wave)
            "...G.....G...", "..GG.....GG..", "..GGGGGGGGG..", "..GYYGYYGGG..",
            "..GGWWKWWGG..", "...GWWWWW....", "....GGWGG.GG.", "...GGGGGGGG..",
            "..G.GGWGG....", "...GGGGGGG...", "....GG.GG....", "....GG.GG....",
            "...WW...WW...", "..WWW...WWW..",

            // 15: Jerry (Pull)
            "..BB.....BB..", ".BBBB...BBBB.", ".BPBB.B.BBPB.", "..BBBBBBBBB..",
            "...BBKBBKB...", "...BBPPPKP...", "....BBPPP....", "....BBBBB....",
            "...BBBBBBB...", "..B.BBPBB.B..", "....BBBBB....", "....BB.BB....",
            "...PP...PP...", "..PPP...PPP..",
            // 15: Jerry (Wave)
            "..BB.....BB..", ".BBBB...BBBB.", ".BPBB.B.BBPB.", "..BBBBBBBBB..",
            "...BBKBBKB...", "...BBPPPKP...", "....BBPPP.BB.", "....BBBBBBB..",
            "...BBBBBB....", "..B.BBPBB....", "....BBBBB....", "....BB.BB....",
            "...PP...PP...", "..PPP...PPP..",

            // 16: Bugs Bunny (Pull)
            "....GG.GG....", "....GP.GP....", "....GG.GG....", "...GGGGGGG...",
            "...GWKGWKG...", "...GWWKWWG...", "....GWWWW....", "....GGWGG....",
            "...GGGGGGG...", "..W.GGWGG.W..", "....GGGGGGG..", "....GG.GG....",
            "...WW...WW...", "..WWW...WWW..",
            // 16: Bugs Bunny (Wave)
            "....GG.GG....", "....GP.GP....", "....GG.GG....", "...GGGGGGG...",
            "...GWKGWKG...", "...GWWKWWG...", "....GWWWW.WW.", "....GGWGGGG..",
            "...GGGGGG....", "..W.GGWGG....", "....GGGGGGG..", "....GG.GG....",
            "...WW...WW...", "..WWW...WWW..",

            // 17: Tweety Bird (Pull)
            ".....Y.Y.....", "....YYYYY....", "...YYYYYYY...", "...YKWYKWY...",
            "...YYYYOYY...", "....YYYYY....", ".....YYY.....", "....YYYYY....",
            "...Y.YYY.Y...", "....YYYYY....", ".....YYY.....", "....O...O....",
            "...OO...OO...", "..OOO...OOO..",
            // 17: Tweety Bird (Wave)
            ".....Y.Y.....", "....YYYYY....", "...YYYYYYY...", "...YKWYKWY...",
            "...YYYYOYY...", "....YYYYY....", ".....YYY..YY.", "....YYYYYYY..",
            "...Y.YYY.....", "....YYYYY....", ".....YYY.....", "....O...O....",
            "...OO...OO...", "..OOO...OOO..",
            // 18: SpongeBob SquarePants (Pull)
            "...YYYYYYY...", "...YEYYYEY...", "...YWKYWKY...", "...YYYYYYY...",
            "...YYRWRYY...", "...YYYYYYY...", "..YYYYYYYYY..", "..YWWWWWWWY..",
            "...BRWRWRB...", "...BBBBBBB...", "....YY.YY....", "....YY.YY....",
            "...KK...KK...", "..KKK...KKK..",
            // 18: SpongeBob SquarePants (Wave)
            "...YYYYYYY...", "...YEYYYEY...", "...YWKYWKY...", "...YYYYYYY...",
            "...YYRWRYY...", "...YYYYYYY...", "..YYYYYYYY.YY", "..YWWWWWWWY..",
            "...BRWRWRB...", "...BBBBBBB...", "....YY.YY....", "....YY.YY....",
            "...KK...KK...", "..KKK...KKK..",

            // 19: Patrick Star (Pull)
            "......M......", ".....MMM.....", "....MMMMM....", "....MKMKM....",
            "...MMMMMMM...", "...MMMRMMM...", "..MMMMMMMMM..", ".M.MMMMMMM.M.",
            "..MMMMMMMMM..", "...EEEEEEE...", "...EUEEEUE...", "...EE...EE...",
            "...MM...MM...", "..MMM...MMM..",
            // 19: Patrick Star (Wave)
            "......M......", ".....MMM.....", "....MMMMM....", "....MKMKM....",
            "...MMMMMMM...", "...MMMRMMM...", "..MMMMMMMM.MM", ".M.MMMMMMM...",
            "..MMMMMMMMM..", "...EEEEEEE...", "...EUEEEUE...", "...EE...EE...",
            "...MM...MM...", "..MMM...MMM..",

            // 20: Scooby-Doo (Pull)
            "...BB...BB...", "..BBBBBBBBB..", "..BBKBBBKBB..", "...BBBBKKK...",
            "....BBBBB....", "....CCCCC....", "...CYYYYYC...", "..BBBBBBBBB..",
            ".B.BBBBBBB.B.", "..BBBBBBBBB..", "...BB...BB...", "...BB...BB...",
            "..BB.....BB..", ".BBB.....BBB.",
            // 20: Scooby-Doo (Wave)
            "...BB...BB...", "..BBBBBBBBB..", "..BBKBBBKBB..", "...BBBBKKK...",
            "....BBBBB....", "....CCCCC.BB.", "...CYYYYYBB..", "..BBBBBBB....",
            ".B.BBBBBBB...", "..BBBBBBBBB..", "...BB...BB...", "...BB...BB...",
            "..BB.....BB..", ".BBB.....BBB.",

            // 21: Garfield (Pull)
            "...OO...OO...", "..OOOOOOOOO..", "..OWWKOOWWK..", "..OOOOOOOOO..",
            "...OOPPPPO...", "....OOOOO....", "...OOOOOOO...", "..O.OKOKO.O..",
            "...OOOOOOO...", "...OOOOOOO...", "....OO.OO....", "....OO.OO....",
            "...OO...OO...", "..OOO...OOO..",
            // 21: Garfield (Wave)
            "...OO...OO...", "..OOOOOOOOO..", "..OWWKOOWWK..", "..OOOOOOOOO..",
            "...OOPPPPO...", "....OOOOO.OO.", "...OOOOOOOO..", "..O.OKOKO....",
            "...OOOOOOO...", "...OOOOOOO...", "....OO.OO....", "....OO.OO....",
            "...OO...OO...", "..OOO...OOO..",

            // 22: Totoro (Pull)
            "...G.....G...", "..GG.....GG..", "..GGGGGGGGG..", "..GWKGGGWKG..",
            "...GGGKGGG...", "..GGGGGGGGG..", ".GGGGGGGGGGG.", "G.GWWWWWWWG.G",
            ".GWWGWWWGWWG.", ".GWWWWWWWWWG.", "..GGGGGGGGG..", "...GGGGGGG...",
            "...GG...GG...", "..GGG...GGG..",
            // 22: Totoro (Wave)
            "...G.....G...", "..GG.....GG..", "..GGGGGGGGG..", "..GWKGGGWKG..",
            "...GGGKGGG...", "..GGGGGGGGG..", ".GGGGGGGGGG.G", "G.GWWWWWWWG..",
            ".GWWGWWWGWWG.", ".GWWWWWWWWWG.", "..GGGGGGGGG..", "...GGGGGGG...",
            "...GG...GG...", "..GGG...GGG..",

            // 23: Doraemon (Pull)
            "....CCCCC....", "...CCCCCCC...", "..CCWWKWWKC..", "..CCWWWRWWC..",
            "..CCWWWWWWC..", "...CWWWWWC...", "....RRRRR....", "...CCYYYCC...",
            "..W.CWWWC.W..", "...CCWWWC....", "....CCCCC....", "....CC.CC....",
            "...WW...WW...", "..WWW...WWW..",
            // 23: Doraemon (Wave)
            "....CCCCC....", "...CCCCCCC...", "..CCWWKWWKC..", "..CCWWWRWWC..",
            "..CCWWWWWWC..", "...CWWWWWC...", "....RRRRR.WW.", "...CCYYYCCWW.",
            "..W.CWWWC....", "...CCWWWC....", "....CCCCC....", "....CC.CC....",
            "...WW...WW...", "..WWW...WWW.."
        }};

        int safe_id = (char_id >= 0 && char_id < 24) ? char_id : 0;
        int base_idx = (safe_id * 2 + (waving ? 1 : 0)) * 14;
        int bob_y = (step_frame % 2 == 1) ? -1 : 0;

        this->set_pen(0x02040A);
        for (int r = 0; r < 14; r++) {
            const char *row_str = sprites.at(base_idx + r);
            int y0 = (r * 24) / 14;
            int y1 = ((r + 1) * 24) / 14;
            for (int c = 0; c < 13; c++) {
                if (*(row_str + c) == '.') continue;
                int x0 = (c * 22) / 13;
                int x1 = ((c + 1) * 22) / 13;

                int dx_anim = (!waving && r >= 12) ? ((step_frame % 2 == 0) ? -1 : 1) : 0;
                int dy_anim = bob_y;
                if (safe_id == 0 && !waving && c <= 3 && r >= 3 && r <= 7) dy_anim += (step_frame % 2 == 0) ? -1 : 0;
                if (waving && c >= 10 && r >= 6 && r <= 8) dy_anim += (step_frame % 2 == 0) ? -1 : 0;

                for (int sy_off = y0 - 1; sy_off <= y1; sy_off++) {
                    for (int sx_off = x0 - 1; sx_off <= x1; sx_off++) {
                        int sx = px_left + sx_off + dx_anim;
                        int sy = py_top + sy_off + dy_anim;
                        if (sx >= (PANEL_W / 2) && sx < PANEL_W && sy >= 0 && sy < PANEL_H) {
                            this->set_pixel(Point(sx, sy));
                        }
                    }
                }
            }
        }

        for (int r = 0; r < 14; r++) {
            const char *row_str = sprites.at(base_idx + r);
            int y0 = (r * 24) / 14;
            int y1 = ((r + 1) * 24) / 14;
            for (int c = 0; c < 13; c++) {
                char ch = *(row_str + c);
                if (ch == '.') continue;

                uint32_t col = 0;
                switch (ch) {
                    case 'K': col = 0x111827; break;
                    case 'W': col = 0xFFFFFF; break;
                    case 'Y': col = 0xFFE600; break;
                    case 'R': col = 0xFF2E2E; break;
                    case 'B': col = 0xA14A0A; break;
                    case 'U': col = 0x2563EB; break;
                    case 'O': col = 0xFF7A00; break;
                    case 'P': col = 0xFFC085; break;
                    case 'G': col = 0xA8B8D0; break;
                    case 'M': col = 0xFF66B2; break;
                    case 'C': col = 0x29C5FF; break;
                    case 'E': col = 0x1ED760; break;
                    default:  col = 0xFFE600; break;
                }

                int x0 = (c * 22) / 13;
                int x1 = ((c + 1) * 22) / 13;

                int dx_anim = (!waving && r >= 12) ? ((step_frame % 2 == 0) ? -1 : 1) : 0;
                int dy_anim = bob_y;
                if (safe_id == 0 && !waving && c <= 3 && r >= 3 && r <= 7) dy_anim += (step_frame % 2 == 0) ? -1 : 0;
                if (waving && c >= 10 && r >= 6 && r <= 8) dy_anim += (step_frame % 2 == 0) ? -1 : 0;

                this->set_pen(col);
                for (int sy_off = y0; sy_off < y1; sy_off++) {
                    for (int sx_off = x0; sx_off < x1; sx_off++) {
                        int sx = px_left + sx_off + dx_anim;
                        int sy = py_top + sy_off + dy_anim;
                        if (sx >= (PANEL_W / 2) && sx < PANEL_W && sy >= 0 && sy < PANEL_H) {
                            this->set_pixel(Point(sx, sy));
                        }
                    }
                }
            }
        }
    }
    void draw_weather_icon(int ox, int oy, int wmo_code, uint32_t flair, int row_idx) {
        bool active_flair = (flair > 0);
        int raw_phase = (int)(MAX_FLAIR_FRAMES - flair) - (row_idx * 3);
        int phase = raw_phase < 0 ? 0 : raw_phase;

        auto put_icon_px = [&](int dx, int dy, uint32_t col) {
            if (dx < 0 || dx > 8 || dy < 0 || dy > 8) return;
            int px = ox + dx;
            int py = oy + dy;
            if (px >= (PANEL_W / 2) && px < PANEL_W && py >= 0 && py < PANEL_H) {
                this->set_pen(col);
                this->set_pixel(Point(px, py));
            }
        };

        if (wmo_code == 0) {
            int pulse = active_flair ? ((phase / 8) % 3) : 0;
            uint32_t core_col = (pulse == 1) ? 0xFFF59D : 0xFFD700;
            uint32_t ray_col  = (pulse == 2) ? 0xFFB703 : 0xFF8C00;

            for (int y = 3; y <= 5; y++)
                for (int x = 3; x <= 5; x++)
                    put_icon_px(x, y, core_col);

            int rot = active_flair ? ((phase / 7) % 4) : 0;
            if (rot == 0) {
                put_icon_px(4, 1, ray_col); put_icon_px(4, 7, ray_col);
                put_icon_px(1, 4, ray_col); put_icon_px(7, 4, ray_col);
            } else if (rot == 1) {
                put_icon_px(5, 1, ray_col); put_icon_px(3, 7, ray_col);
                put_icon_px(1, 3, ray_col); put_icon_px(7, 5, ray_col);
            } else if (rot == 2) {
                put_icon_px(6, 2, ray_col); put_icon_px(2, 6, ray_col);
                put_icon_px(2, 2, ray_col); put_icon_px(6, 6, ray_col);
            } else {
                put_icon_px(3, 1, ray_col); put_icon_px(5, 7, ray_col);
                put_icon_px(1, 5, ray_col); put_icon_px(7, 3, ray_col);
            }
        }
        else if (wmo_code <= 3) {
            static const std::array<int, 4> drift_seq = {{0, 1, 0, -1}};
            int drift = active_flair ? drift_seq.at((phase / 10) % 4) : 0;

            if (wmo_code <= 2) {
                uint32_t sun_col = (active_flair && ((phase / 12) % 2 == 0)) ? 0xFFE066 : 0xFFD700;
                put_icon_px(6, 1, sun_col); put_icon_px(7, 1, sun_col);
                put_icon_px(6, 2, sun_col); put_icon_px(7, 2, sun_col);
            }

            uint32_t cloud_col = (wmo_code == 3) ? 0x9BAEC8 : 0xE2E8F0;
            for (int x = 2; x <= 5; x++) put_icon_px(x + drift, 3, cloud_col);
            for (int x = 1; x <= 7; x++) put_icon_px(x + drift, 4, cloud_col);
            for (int x = 1; x <= 7; x++) put_icon_px(x + drift, 5, cloud_col);
        }
        else if (wmo_code == 45 || wmo_code == 48) {
            static const std::array<int, 4> mist_seq = {{0, 1, 2, 1}};
            int shift = active_flair ? mist_seq.at((phase / 9) % 4) : 0;
            uint32_t fog1 = 0x94A3B8;
            uint32_t fog2 = 0xCBD5E1;
            for (int x = 1; x <= 5; x++) put_icon_px(x + shift, 2, fog1);
            for (int x = 3; x <= 7; x++) put_icon_px(x - shift, 4, fog2);
            for (int x = 1; x <= 5; x++) put_icon_px(x + shift, 6, fog1);
        }
        else if ((wmo_code >= 71 && wmo_code <= 77) || wmo_code == 85 || wmo_code == 86) {
            for (int x = 2; x <= 6; x++) put_icon_px(x, 1, 0xCBD5E1);
            for (int x = 1; x <= 7; x++) put_icon_px(x, 2, 0xE2E8F0);
            for (int x = 1; x <= 7; x++) put_icon_px(x, 3, 0xE2E8F0);

            int step = active_flair ? ((phase / 7) % 4) : 0;
            int sway = (step == 1) ? 1 : (step == 3 ? -1 : 0);

            int y1 = 4 + step;
            int y2 = 4 + ((step + 2) % 4);
            int y3 = 4 + ((step + 1) % 4);

            put_icon_px(2 + sway, y1, 0xFFFFFF);
            put_icon_px(4 - sway, y2, 0x7DD3FC);
            put_icon_px(6 + sway, y3, 0xFFFFFF);
        }
        else if (wmo_code >= 95) {
            for (int x = 2; x <= 6; x++) put_icon_px(x, 1, 0x64748B);
            for (int x = 1; x <= 7; x++) put_icon_px(x, 2, 0x475569);
            for (int x = 1; x <= 7; x++) put_icon_px(x, 3, 0x475569);

            int strike = active_flair ? ((phase / 5) % 5) : 2;
            if (strike != 0) {
                uint32_t bolt = (strike == 1 || strike == 3) ? 0xFFFFFF : 0xFACC15;
                put_icon_px(4, 4, bolt);
                put_icon_px(3, 5, bolt);
                put_icon_px(4, 5, bolt);
                if (strike >= 2) {
                    put_icon_px(5, 6, bolt);
                    put_icon_px(4, 7, bolt);
                }
            }
        }
        else {
            int bob = (active_flair && ((phase / 12) % 2 == 1)) ? 1 : 0;
            for (int x = 2; x <= 6; x++) put_icon_px(x, 1 + bob, 0x94A3B8);
            for (int x = 1; x <= 7; x++) put_icon_px(x, 2 + bob, 0xCBD5E1);
            for (int x = 1; x <= 7; x++) put_icon_px(x, 3 + bob, 0xCBD5E1);

            int fall = active_flair ? ((phase / 4) % 4) : 1;
            int d1 = fall;
            int d2 = (fall + 2) % 4;
            int d3 = (fall + 1) % 4;

            auto draw_drop = [&](int start_x, int stage) {
                int rx = start_x - (stage / 2);
                int ry = 4 + stage;
                if (stage < 3) {
                    put_icon_px(rx, ry, 0x38BDF8);
                    put_icon_px(rx, ry + 1, 0x0284C7);
                } else {
                    put_icon_px(rx - 1, 8, 0x7DD3FC);
                    put_icon_px(rx + 1, 8, 0x7DD3FC);
                }
            };

            draw_drop(3, d1);
            draw_drop(5, d2);
            draw_drop(7, d3);
        }
    }
    void draw_weather_panel() {
        this->set_pen(0);
        for (int y = 0; y < PANEL_H; y++) {
            for (int x = 0; x < (PANEL_W / 2); x++) {
                this->set_pixel(Point((PANEL_W / 2) + x, y));
            }
        }

        if (!has_weather_location()) {
            return;
        }

        if (!weather_valid) {
            draw_tiny_str((PANEL_W / 2) + 6, 20, "12h", 0x38BDF8);
            draw_tiny_str((PANEL_W / 2) + 22, 20, "...", 0x94A3B8);
            return;
        }

        bool use_cartoons = is_cartoons_enabled();
        uint32_t anim_total_frames = use_cartoons ? MAX_FLAIR_FRAMES : 90;

        if (weather_just_updated) {
            weather_just_updated = false;
            if (weather_flair_frame == 0) {
                if (use_cartoons) {
                    int next_char = (int)((to_us_since_boot(get_absolute_time()) + rand()) % 24);
                    if (next_char == active_character) {
                        next_char = (next_char + 1) % 24;
                    }
                    active_character = next_char;
                }
                weather_flair_frame = anim_total_frames;
            }
        }

        int elapsed = (int)(anim_total_frames - weather_flair_frame);
        bool show_character = (use_cartoons && weather_flair_frame > 0);
        int content_shift_x = 0;
        int character_x = (PANEL_W / 2) + 25;
        int character_y = 23;
        bool character_waving = false;

        if (weather_flair_frame > 0) {
            if (use_cartoons) {
                if (elapsed <= 195) {
                    content_shift_x = -36 + (elapsed * 36) / 195;
                    character_x = (PANEL_W / 2) + 25 + content_shift_x;
                    character_waving = false;
                } else if (elapsed <= 300) {
                    content_shift_x = 0;
                    character_x = (PANEL_W / 2) + 25;
                    character_waving = true;
                } else {
                    content_shift_x = 0;
                    character_x = (PANEL_W / 2) + 25 + ((elapsed - 300) * 24) / 60;
                    character_y = 23 - (((elapsed - 300) / 7) % 2) * 2;
                    character_waving = false;
                }
            } else {
                int slide_frames = (int)anim_total_frames;
                if (elapsed < slide_frames) {
                    content_shift_x = 48 - (elapsed * 48) / slide_frames;
                }
            }
        }

        bool render_daily = (show_daily_forecast && daily_weather_valid);

        for (int i = 0; i < 4; i++) {
            int row_y = i * 12;
            int base_x = (PANEL_W / 2) + content_shift_x;

            if (i > 0) {
                this->set_pen(0x1E293B);
                for (int dx = 1; dx < (PANEL_W / 2) - 1; dx++) {
                    int lx = base_x + dx;
                    if (lx >= (PANEL_W / 2) && lx < PANEL_W) {
                        this->set_pixel(Point(lx, row_y));
                    }
                }
            }

            if (render_daily) {
                const char *day_lbl = day_of_week_str(
                    forecast_4d.at(i).year,
                    forecast_4d.at(i).month,
                    forecast_4d.at(i).day
                );
                draw_tiny_str(base_x + 0, row_y + 4, day_lbl, (i == 0) ? 0x38BDF8 : 0x94A3B8);

                draw_weather_icon(base_x + 13, row_y + 2, forecast_4d.at(i).wmo_code, weather_flair_frame, i);

                std::array<char, 6> max_str;
                std::array<char, 6> min_str;
                snprintf(max_str.data(), max_str.size(), "%d", forecast_4d.at(i).temp_max);
                snprintf(min_str.data(), min_str.size(), "%d", forecast_4d.at(i).temp_min);

                int max_w = (int)strlen(max_str.data()) * 4;
                int slash_w = 4;
                int min_w = (int)strlen(min_str.data()) * 4 - 1;
                int total_w = max_w + slash_w + min_w;
                int temp_x = base_x + (PANEL_W / 2) - total_w - 3;

                uint32_t max_col = (forecast_4d.at(i).temp_max >= 28) ? 0xFB923C : 0xFACC15;
                draw_tiny_str(temp_x, row_y + 4, max_str.data(), max_col);
                draw_tiny_str(temp_x + max_w, row_y + 4, "/", 0x64748B);
                draw_tiny_str(temp_x + max_w + slash_w, row_y + 4, min_str.data(), 0x7DD3FC);
            } else {
                std::array<char, 8> time_lbl;
                snprintf(time_lbl.data(), time_lbl.size(), "%02dh", forecast_12h.at(i).hour);
                draw_tiny_str(base_x + 0, row_y + 4, time_lbl.data(), (i == 0) ? 0xF8FAFC : 0x94A3B8);

                draw_weather_icon(base_x + 13, row_y + 2, forecast_12h.at(i).wmo_code, weather_flair_frame, i);

                std::array<char, 8> temp_str;
                snprintf(temp_str.data(), temp_str.size(), "%d*", forecast_12h.at(i).temp_c);
                uint32_t temp_col = (forecast_12h.at(i).temp_c >= 28) ? 0xFB923C :
                                    (forecast_12h.at(i).temp_c <= 8)  ? 0x7DD3FC : 0xFACC15;
                draw_tiny_str(base_x + 23, row_y + 4, temp_str.data(), temp_col);

                std::array<char, 8> rain_str;
                snprintf(rain_str.data(), rain_str.size(), "%d%%", forecast_12h.at(i).rain_prob);
                int str_px_w = (int)strlen(rain_str.data()) * 4 - 1;
                int rain_x = base_x + (PANEL_W / 2) - str_px_w - 2;
                uint32_t rain_col = (forecast_12h.at(i).rain_prob >= 50) ? 0x38BDF8 : 0x64748B;
                draw_tiny_str(rain_x, row_y + 4, rain_str.data(), rain_col);
            }

            if (show_character && elapsed <= 195) {
                int rope_start_x = base_x + (PANEL_W / 2);
                int rope_end_x = character_x + 4;
                int rope_y = row_y + 6;
                this->set_pen(0xD97706);
                for (int rx = rope_start_x; rx <= rope_end_x; rx++) {
                    if (rx >= (PANEL_W / 2) && rx < PANEL_W) {
                        this->set_pixel(Point(rx, rope_y));
                    }
                }
            }
        }

        if (show_character) {
            draw_character(active_character, character_x, character_y, elapsed / 10, character_waving);
        }

        if (weather_flair_frame > 0) {
            weather_flair_frame--;
        }
    }
    void draw_snoopy_walking_side(int px_left, int py_top, int step_frame) {
        static const std::array<const char*, 15> walk_sprite = {{
            "......WWWWW.......",
            ".....WWWWWWWW.....",
            "....WWWWWWKWWW....",
            "...KKWWWWWWWWWWKK.",
            "..KKKKWWWWWWWWWWK.",
            "..KKKK.WWWWWWWWW..",
            "...KK...WWWWWW....",
            "........RRRRR.....",
            "......WWWWWWWW....",
            "....KKWWWWWWWWW...",
            "..W.KKWWWWWWWWW...",
            ".....WWWWWWWWW....",
            "......WWWWWWW.....",
            ".......WW..WW.....",
            "......WWW..WWW...."
        }};

        int bob_y = (step_frame % 2 == 1) ? -1 : 0;
        for (int r = 0; r < 15; r++) {
            const char *row_str = walk_sprite.at(r);
            for (int c = 0; c < 18; c++) {
                char ch = *(row_str + c);
                if (ch == '.') continue;

                uint32_t col = 0xFFFFFF;
                if (ch == 'K') col = 0x111827;
                else if (ch == 'R') col = 0xEF4444;

                int dx = c;
                int dy = r + bob_y;
                if (r >= 13) {
                    if (c < 9) dx += (step_frame % 2 == 0) ? -1 : 1;
                    else       dx += (step_frame % 2 == 0) ? 1 : -1;
                }
                if (r >= 9 && r <= 11 && c <= 3) {
                    dy += (step_frame % 2 == 0) ? -1 : 0;
                }

                int sx = px_left + dx;
                int sy = py_top + dy;
                if (sx >= 0 && sx < PANEL_W && sy >= 0 && sy < PANEL_H) {
                    this->set_pen(col);
                    this->set_pixel(Point(sx, sy));
                }
            }
        }
    }

    void draw_woodstock_perched(int px_left, int py_top, int breath_offset) {
        static const std::array<const char*, 7> woodstock_sprite = {{
            ".Y.Y...",
            "..YYY..",
            ".YYKYYO",
            "..YYOOO",
            "..YYYY.",
            "...YY..",
            "..OO..."
        }};

        for (int r = 0; r < 7; r++) {
            const char *row_str = woodstock_sprite.at(r);
            for (int c = 0; c < 7; c++) {
                char ch = *(row_str + c);
                if (ch == '.') continue;

                uint32_t col = 0xFFE600;
                if (ch == 'K') col = 0x111827;
                else if (ch == 'O') col = 0xF97316;

                int sx = px_left + c;
                int sy = py_top + r + breath_offset;
                if (sx >= 0 && sx < PANEL_W && sy >= 0 && sy < PANEL_H) {
                    this->set_pen(col);
                    this->set_pixel(Point(sx, sy));
                }
            }
        }
    }
    void draw_snoopy_sleeping_top(int px_left, int py_top, uint32_t sleep_ticks) {
        static const std::array<const char*, 10> sleep_sprite = {{
            "................KK..",
            "...............WWWW.",
            "...............WWWW.",
            "..WW..........WWWWWW",
            ".WWWW........WWWWKWW",
            ".WWWW...RRRRWWWWWWWW",
            "..WWWWWWWWWWWWWWKKK.",
            "...WWWWWWWWWWWWWKKKK",
            "....WWWWWWWWWWWW.KK.",
            "....................",
        }};

        for (int r = 0; r < 10; r++) {
            const char *row_str = sleep_sprite.at(r);
            for (int c = 0; c < 20; c++) {
                char ch = *(row_str + c);
                if (ch == '.') continue;

                uint32_t col = 0xFFFFFF;
                if (ch == 'K') col = 0x111827;
                else if (ch == 'R') col = 0xEF4444;

                int sx = px_left + c;
                int sy = py_top + r;
                if (sx >= 0 && sx < PANEL_W && sy >= 0 && sy < PANEL_H) {
                    this->set_pen(col);
                    this->set_pixel(Point(sx, sy));
                }
            }
        }

        int breath_offset = ((sleep_ticks / 45) % 2 == 1) ? -1 : 0;
        draw_woodstock_perched(px_left + 6, py_top - 1, breath_offset);
    }

    void draw_snoopy_bedtime_scene() {
        this->set_pen(0x030712);
        for (int y = 0; y < 43; y++) {
            for (int x = 0; x < PANEL_W; x++) {
                this->set_pixel(Point(x, y));
            }
        }
        this->set_pen(0x14532D);
        for (int y = 43; y < PANEL_H; y++) {
            for (int x = 0; x < PANEL_W; x++) {
                this->set_pixel(Point(x, y));
            }
        }

        this->set_pen(0xFDE047);
        for (int dy = -4; dy <= 4; dy++) {
            for (int dx = -4; dx <= 4; dx++) {
                if (dx * dx + dy * dy <= 16 && (dx + 2) * (dx + 2) + dy * dy > 11) {
                    this->set_pixel(Point(12 + dx, 10 + dy));
                }
            }
        }

        uint32_t tick = to_ms_since_boot(get_absolute_time()) / 250;
        static const std::array<int, 16> stars = {{
            26, 6,  38, 12,  52, 5,  66, 11,  82, 7,  32, 20,  47, 17,  60, 19
        }};
        for (int i = 0; i < 8; i++) {
            int sx = stars.at(i * 2);
            int sy = stars.at(i * 2 + 1);
            this->set_pen(((tick + i) % 3 == 0) ? 0x64748B : 0xF8FAFC);
            this->set_pixel(Point(sx, sy));
        }

        int kx = 64;
        this->set_pen(0xDC2626);
        for (int ry = 0; ry <= 6; ry++) {
            int half_w = 5 + ry;
            for (int rx = 13 - half_w; rx <= 13 + half_w; rx++) {
                this->set_pixel(Point(kx + rx, 24 + ry));
            }
        }
        this->set_pen(0xFCA5A5);
        for (int rx = 8; rx <= 18; rx++) {
            this->set_pixel(Point(kx + rx, 24));
        }
        this->set_pen(0xB91C1C);
        for (int wy = 31; wy <= 42; wy++) {
            for (int wx = 3; wx <= 22; wx++) {
                this->set_pixel(Point(kx + wx, wy));
            }
        }
        this->set_pen(0x090D16);
        for (int dy = 34; dy <= 42; dy++) {
            for (int dx = 9; dx <= 16; dx++) {
                if (dy == 34 && (dx == 9 || dx == 16)) continue;
                this->set_pixel(Point(kx + dx, dy));
            }
        }

        static uint32_t bedtime_frame = 0;
        if (!snoopy_bedtime_active) {
            bedtime_frame = 0;
            return;
        }

        if (bedtime_frame <= 750) {
            int sx = -14 + (int)((bedtime_frame * 62) / 750);
            int sy = 28;
            draw_snoopy_walking_side(sx, sy, (int)(bedtime_frame / 10));
        } else if (bedtime_frame <= 975) {
            uint32_t climb_t = bedtime_frame - 750;
            int sx = 48 + (int)((climb_t * 20) / 225);
            int arc = (int)((climb_t * (225 - climb_t)) / 2500);
            int sy = 28 - (int)((climb_t * 13) / 225) - arc;
            draw_snoopy_walking_side(sx, sy, (int)(bedtime_frame / 8));
        } else {
            uint32_t sleep_ticks = bedtime_frame - 975;
            draw_snoopy_sleeping_top(67, 15, sleep_ticks);

            int z_cycle = (int)(sleep_ticks / 12);
            for (int z = 0; z < 3; z++) {
                int age = (z_cycle + z * 5) % 15;
                int zx = 83 + (age / 3);
                int zy = 14 - age;
                if (zy >= 1 && zy < 15) {
                    uint32_t z_col = (age < 10) ? 0x93C5FD : 0x3B82F6;
                    this->set_pen(z_col);
                    this->set_pixel(Point(zx, zy));
                    this->set_pixel(Point(zx + 1, zy));
                    this->set_pixel(Point(zx + 2, zy));
                    this->set_pixel(Point(zx + 1, zy + 1));
                    this->set_pixel(Point(zx, zy + 2));
                    this->set_pixel(Point(zx + 1, zy + 2));
                    this->set_pixel(Point(zx + 2, zy + 2));
                }
            }
        }

        bedtime_frame++;
        if (bedtime_frame > 1350) {
            bedtime_frame = 0;
            snoopy_bedtime_active = false;
        }
    }
public:
    explicit AnalogClock()
    {
        centerX = (float)(W / 2);
        centerY = (float)(H / 2);
        l = std::min(W, H);
        r = l * (0.95f / 2.0f);

        for (auto i = 15; i < 75; i++)
        {
            s.at(i - 15) = std::sin(i * da) * r;
            c.at(i - 15) = std::cos(i * da) * r;
        }

        struct timespec ts = {0LL, 0L};
        ts.tv_sec = 60 * 60 * 3 + 25 * 60 + 45;

        if (!aon_timer_start(&ts))
        {
            printf("Could not set AON timer");
        }

        this->set_font(&font14_outline);
    }

    void clear_left() {
        for (int x = 0; x < 48; x++) {
            for (int y = 0; y < 48; y++) {
                this->set_pen(0);
                this->set_pixel(Point(x, y)); 
            }
        }
    }

    void transition_right() {
        if (is_all_zero && !is_transitioning) {
            image_ready = false;
            draw_weather_panel();
            return;
        }

        if (image_ready && !is_transitioning) {
            image_ready = false;
            is_transitioning = true;
            transition_step = 0;
            active_transition = rand() % 10;
        }

        if (is_transitioning) {
            for (int y = 0; y < PANEL_H; ++y) {
                for (int x = 0; x < (PANEL_W / 2); ++x) {
                    size_t idx = (y * (PANEL_W / 2) + x) * 3;

                    uint8_t r = 0, g = 0, b = 0;

                    uint8_t r_old = current_image_buf[idx];
                    uint8_t g_old = current_image_buf[idx + 1];
                    uint8_t b_old = current_image_buf[idx + 2];

                    uint8_t r_new = image_buf[idx];
                    uint8_t g_new = image_buf[idx + 1];
                    uint8_t b_new = image_buf[idx + 2];

                    switch (active_transition) {
                        case 0:
                            r = ((r_old * (TOTAL_STEPS - transition_step)) + (r_new * transition_step)) / TOTAL_STEPS;
                            g = ((g_old * (TOTAL_STEPS - transition_step)) + (g_new * transition_step)) / TOTAL_STEPS;
                            b = ((b_old * (TOTAL_STEPS - transition_step)) + (b_new * transition_step)) / TOTAL_STEPS;
                            break;

                        case 1:
                            if (transition_step > (uint32_t)((y * TOTAL_STEPS) / PANEL_H)) {
                                r = r_new; g = g_new; b = b_new;
                            } else {
                                r = r_old; g = g_old; b = b_old;
                            }
                            break;

                        case 2:
                            {
                                int slide_offset = (transition_step * (PANEL_W / 2)) / TOTAL_STEPS;
                                if (x < slide_offset) {
                                    size_t new_idx = (y * (PANEL_W / 2) + (x + (PANEL_W / 2) - slide_offset)) * 3;
                                    r = image_buf[new_idx]; g = image_buf[new_idx + 1]; b = image_buf[new_idx + 2];
                                } else {
                                    size_t old_idx = (y * (PANEL_W / 2) + (x - slide_offset)) * 3;
                                    r = current_image_buf[old_idx]; g = current_image_buf[old_idx + 1]; b = current_image_buf[old_idx + 2];
                                }
                            }
                            break;

                        case 3:
                            {
                                int center_x = (PANEL_W / 2) / 2;
                                int curtain_width = (transition_step * center_x) / TOTAL_STEPS;
                                if (x >= (center_x - curtain_width) && x <= (center_x + curtain_width)) {
                                    r = r_new; g = g_new; b = b_new;
                                } else {
                                    r = r_old; g = g_old; b = b_old;
                                }
                            }
                            break;

                        case 4:
                            if ((uint32_t)(((x + y) * TOTAL_STEPS) / ((PANEL_W / 2) + PANEL_H)) < transition_step) {
                                r = r_new; g = g_new; b = b_new;
                            } else {
                                r = r_old; g = g_old; b = b_old;
                            }
                            break;
                        case 5:
                            if ((hash_coor(x, y) % TOTAL_STEPS) < transition_step) {
                                r = r_new; g = g_new; b = b_new;
                            } else {
                                r = r_old; g = g_old; b = b_old;
                            }
                            break;

                        case 6:
                            {
                                int bx = x / 4; int by = y / 4;
                                if ((hash_coor(bx, by) % TOTAL_STEPS) < transition_step) {
                                    r = r_new; g = g_new; b = b_new;
                                } else {
                                    r = r_old; g = g_old; b = b_old;
                                }
                            }
                            break;

                        case 7:
                            {
                                int cx = (PANEL_W / 2) / 2; int cy = PANEL_H / 2;
                                int dx = x - cx; int dy = y - cy;
                                int dist_sq = dx*dx + dy*dy;
                                int max_radius_sq = cx*cx + cy*cy;
                                int current_radius_sq = (transition_step * max_radius_sq) / TOTAL_STEPS;
                                if (dist_sq <= current_radius_sq) {
                                    r = r_new; g = g_new; b = b_new;
                                } else {
                                    r = r_old; g = g_old; b = b_old;
                                }
                            }
                            break;

                        case 8:
                            {
                                uint32_t half_steps = TOTAL_STEPS / 2;
                                if (transition_step <= half_steps) { 
                                    r = (r_old * (half_steps - transition_step)) / half_steps;
                                    g = (g_old * (half_steps - transition_step)) / half_steps;
                                    b = (b_old * (half_steps - transition_step)) / half_steps;
                                } else { 
                                    uint32_t current_substep = transition_step - half_steps;
                                    r = (r_new * current_substep) / half_steps;
                                    g = (g_new * current_substep) / half_steps;
                                    b = (b_new * current_substep) / half_steps;
                                }
                            }
                            break;

                        case 9:
                            if (transition_step > (uint32_t)(((PANEL_H - y) * TOTAL_STEPS) / PANEL_H)) {
                                r = r_new; g = g_new; b = b_new;
                            } else {
                                r = r_old; g = g_old; b = b_old;
                            }
                            break;
                    }
                    this->set_pen(this->create_pen(r, g, b));
                    this->set_pixel(Point((PANEL_W / 2) + x, y));
                }
            }

            transition_step++;

            if (transition_step > TOTAL_STEPS) {
                is_transitioning = false;
                memcpy(current_image_buf, image_buf, IMG_SIZE);
                memset(image_buf, 0, IMG_SIZE);
            }
        }
        else {
            for (int y = 0; y < PANEL_H; ++y) {
                for (int x = 0; x < (PANEL_W / 2); ++x) {
                    size_t idx = (y * (PANEL_W / 2) + x) * 3;

                    uint8_t r = current_image_buf[idx];
                    uint8_t g = current_image_buf[idx + 1];
                    uint8_t b = current_image_buf[idx + 2];

                    this->set_pen(this->create_pen(r, g, b));
                    this->set_pixel(Point((PANEL_W / 2) + x, y));
                }
            }
        }
    }
    void draw()
    {
        if (snoopy_bedtime_active) {
            draw_snoopy_bedtime_scene();
            previous_second = -1;
            return;
        }

        struct timespec ct;
        
        if (aon_timer_get_time(&ct))
        {
            int period = (ct.tv_sec % (3600 * 12));
            hour = (ct.tv_sec % (3600 * 12)) / 3600;
            minute = (period - hour * 3600) / 60;
            second = (period - hour * 3600 - minute * 60);
        }

        if (second != previous_second)
        {
            previous_second = second;
            this->set_pen(0);

            clear_left();

            this->set_pen(clock_digits);

            int shiftLeft = 26; 
            int localCenterX = centerX - shiftLeft;

            if constexpr (W > 32)
            {   
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
                this->drawLine(c.at(i * 5) + localCenterX, s.at(i * 5) + centerY, c.at(i * 5) * 0.8f + localCenterX, s.at(i * 5) * 0.8f + centerY, main_ticks);
            }

            int corrected_hour = hour * 5 + minute / 12;
            this->drawLine(c.at(corrected_hour) * 0.1f + localCenterX + 1, s.at(corrected_hour) * 0.2f + centerY + 1, -c.at(corrected_hour) * 0.6f + localCenterX, -s.at(corrected_hour) * 0.6f + centerY, shadow_hand);
            this->drawLine(c.at(corrected_hour) * 0.1f + localCenterX, s.at(corrected_hour) * 0.2f + centerY, -c.at(corrected_hour) * 0.6f + localCenterX, -s.at(corrected_hour) * 0.6f + centerY, hour_hand);
            this->drawLine(c.at(minute) * 0.15f + localCenterX + 1, s.at(minute) * 0.2f + centerY + 1, -c.at(minute) * 0.85f + localCenterX, -s.at(minute) * 0.85f + centerY, shadow_hand);
            this->drawLine(c.at(minute) * 0.15f + localCenterX, s.at(minute) * 0.2f + centerY, -c.at(minute) * 0.85f + localCenterX, -s.at(minute) * 0.85f + centerY, minute_hand);
            this->drawLine(c.at(second) * 0.1f + localCenterX + 1, s.at(second) * 0.1f + centerY + 1, -c.at(second) * 0.85f + localCenterX, -s.at(second) * 0.85f + centerY, second_shadow_hand);
            this->drawLine(c.at(second) * 0.1f + localCenterX, s.at(second) * 0.1f + centerY, -c.at(second) * 0.85f + localCenterX, -s.at(second) * 0.85f + centerY, second_hand);
        }
        transition_right();
    }
};
int main()
{
    initialize();

    static AnalogClock<Panel::SCREEN_WIDTH, Panel::SCREEN_HEIGHT> analogClock;

    const float fps = 75.0f;
    const float frame_delay_ms = 1000.0f / fps;

    driver.setBasisBrightness(3);
    float intensity = 0.5f;
    driver.setIntensity(intensity);

    fetch_weather();

    int last_weather_hour = -1;
    struct timespec init_ct;
    if (mutex_enter_timeout_ms(&aon_timer_mutex, 10)) {
        if (aon_timer_get_time(&init_ct)) {
            last_weather_hour = (init_ct.tv_sec % 86400) / 3600;
        }
        mutex_exit(&aon_timer_mutex);
    }

    absolute_time_t next_frame = make_timeout_time_ms((uint32_t)frame_delay_ms);
    absolute_time_t next_wifi_check_time = make_timeout_time_us(CHECK_INTERVAL_US);

    while (true)
    {
        analogClock.draw();
        driver.update(&analogClock);

        struct timespec now_ct;
        if (mutex_enter_timeout_ms(&aon_timer_mutex, 2)) {
            bool got_time = aon_timer_get_time(&now_ct);
            mutex_exit(&aon_timer_mutex);

            if (got_time) {
                int cur_day = now_ct.tv_sec / 86400;
                int sec_in_day = now_ct.tv_sec % 86400;
                int cur_hour = sec_in_day / 3600;
                int sec_in_hour = sec_in_day % 3600;
                int sec_in_quarter = sec_in_day % 900;

                int bed_hour = -1;
                int bed_min = -1;
                if (parse_bed_time(bed_hour, bed_min)) {
                    int cur_min = sec_in_hour / 60;
                    if (cur_hour == bed_hour && cur_min == bed_min && cur_day != last_snoopy_day) {
                        last_snoopy_day = cur_day;
                        snoopy_bedtime_active = true;
                        printf("\n--- %02d:%02d Snoopy Bedtime Animation ---\n", bed_hour, bed_min);
                    }
                }

                if (has_weather_location()) {
                    bool should_show_daily = (daily_weather_valid && sec_in_quarter < 120);
                    if (should_show_daily != show_daily_forecast) {
                        show_daily_forecast = should_show_daily;
                        if (is_all_zero) {
                            weather_just_updated = true;
                        }
                    }

                    if (cur_hour != last_weather_hour && sec_in_hour >= 120) {
                        last_weather_hour = cur_hour;
                        printf("\n--- Hourly Weather Update at %02d:02 ---\n", cur_hour);
                        if (is_all_zero) {
                            fetch_weather();
                        }
                    }
                }
            }
        }

        if (time_reached(next_wifi_check_time)) {
            printf("\n--- 5-Minute Link Status Check ---\n");
            int link_status = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);

            if (link_status == CYW43_LINK_UP) {
                printf("Status: Connected. Signal OK.\n");
            } else {
                printf("Status: Disconnected! (Code: %d). Reconnecting...\n", link_status);
                connect_wifi();
            }

            next_wifi_check_time = make_timeout_time_us(CHECK_INTERVAL_US);
        }

        sleep_until(next_frame);
        next_frame = delayed_by_ms(next_frame, (uint32_t)frame_delay_ms);
    }
}
