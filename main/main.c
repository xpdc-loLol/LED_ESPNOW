#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"
#include "nvs_flash.h"

#define RGB_LED_GPIO GPIO_NUM_48
#define OLED_SDA_GPIO GPIO_NUM_8
#define OLED_SCL_GPIO GPIO_NUM_9
#define OLED_I2C_PORT I2C_NUM_0
#define OLED_I2C_ADDRESS 0x3C
#define OLED_WIDTH 128
#define OLED_PAGES 8

static const char *TAG = "led_web";
static led_strip_handle_t rgb_led;
static SemaphoreHandle_t led_mutex;
static char current_color[12] = "OFF";
static char current_ip[16] = "0.0.0.0";
static uint8_t base_red;
static uint8_t base_green;
static uint8_t base_blue;
static uint8_t brightness = 255;
static uint8_t output_red;
static uint8_t output_green;
static uint8_t output_blue;
static bool wifi_connected;

// The application only needs a small uppercase/digit subset for the OLED labels.
static void glyph_for_char(char character, uint8_t glyph[5])
{
	static const uint8_t blank[5] = {0, 0, 0, 0, 0};
	static const uint8_t digits[10][5] = {
		{0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
		{0x62, 0x51, 0x49, 0x49, 0x46}, {0x22, 0x49, 0x49, 0x49, 0x36},
		{0x18, 0x14, 0x12, 0x7F, 0x10}, {0x2F, 0x49, 0x49, 0x49, 0x31},
		{0x3E, 0x49, 0x49, 0x49, 0x32}, {0x01, 0x71, 0x09, 0x05, 0x03},
		{0x36, 0x49, 0x49, 0x49, 0x36}, {0x26, 0x49, 0x49, 0x49, 0x3E}
	};
	static const uint8_t letters[26][5] = {
		{0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
		{0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C},
		{0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
		{0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
		{0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01},
		{0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
		{0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
		{0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06},
		{0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
		{0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
		{0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
		{0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
		{0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}
	};

	memcpy(glyph, blank, sizeof(blank));
	if (character >= '0' && character <= '9') {
		memcpy(glyph, digits[character - '0'], 5);
	} else if (character >= 'A' && character <= 'Z') {
		memcpy(glyph, letters[character - 'A'], 5);
	} else if (character == ':') {
		glyph[1] = 0x36;
		glyph[3] = 0x36;
	} else if (character == '.') {
		glyph[4] = 0x60;
	}
}

static esp_err_t oled_command(uint8_t command)
{
	uint8_t data[] = {0x00, command};
	return i2c_master_write_to_device(OLED_I2C_PORT, OLED_I2C_ADDRESS, data,
									  sizeof(data), pdMS_TO_TICKS(100));
}

static void oled_init(void)
{
	i2c_config_t config = {
		.mode = I2C_MODE_MASTER,
		.sda_io_num = OLED_SDA_GPIO,
		.scl_io_num = OLED_SCL_GPIO,
		.sda_pullup_en = GPIO_PULLUP_ENABLE,
		.scl_pullup_en = GPIO_PULLUP_ENABLE,
		.master.clk_speed = 400000,
	};
	ESP_ERROR_CHECK(i2c_param_config(OLED_I2C_PORT, &config));
	ESP_ERROR_CHECK(i2c_driver_install(OLED_I2C_PORT, config.mode, 0, 0, 0));

	const uint8_t init_commands[] = {0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00,
		0x40, 0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF,
		0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF};
	for (size_t index = 0; index < sizeof(init_commands); index++) {
		ESP_ERROR_CHECK(oled_command(init_commands[index]));
	}
}

static void oled_clear(void)
{
	uint8_t buffer[OLED_WIDTH + 1] = {0x40};
	for (int page = 0; page < OLED_PAGES; page++) {
		ESP_ERROR_CHECK(oled_command(0xB0 + page));
		ESP_ERROR_CHECK(oled_command(0x00));
		ESP_ERROR_CHECK(oled_command(0x10));
		ESP_ERROR_CHECK(i2c_master_write_to_device(OLED_I2C_PORT, OLED_I2C_ADDRESS,
													buffer, sizeof(buffer), pdMS_TO_TICKS(100)));
	}
}

static void oled_write_line(int page, const char *text)
{
	uint8_t buffer[OLED_WIDTH + 1] = {0x40};
	int column = 0;
	for (const char *character = text; *character != '\0' && column < OLED_WIDTH - 5; character++) {
		uint8_t glyph[5];
		glyph_for_char(*character, glyph);
		memcpy(&buffer[column + 1], glyph, 5);
		column += 6;
	}
	oled_command(0xB0 + page);
	oled_command(0x00);
	oled_command(0x10);
	i2c_master_write_to_device(OLED_I2C_PORT, OLED_I2C_ADDRESS, buffer, sizeof(buffer), pdMS_TO_TICKS(100));
}

static void oled_show_status(void)
{
	char ip_line[22];
	char rgb_line[20];
	snprintf(ip_line, sizeof(ip_line), "IP:%s", current_ip);
	snprintf(rgb_line, sizeof(rgb_line), "RGB:%u,%u,%u", output_red, output_green, output_blue);
	oled_clear();
	oled_write_line(0, wifi_connected ? "WIFI CONNECTED" : "WIFI CONNECTING");
	oled_write_line(2, ip_line);
	oled_write_line(4, current_color);
	oled_write_line(6, rgb_line);
}

static void apply_led_state(const char *color)
{
	uint8_t red = ((uint16_t)base_red * brightness) / 255;
	uint8_t green = ((uint16_t)base_green * brightness) / 255;
	uint8_t blue = ((uint16_t)base_blue * brightness) / 255;

	xSemaphoreTake(led_mutex, portMAX_DELAY);
	ESP_ERROR_CHECK(led_strip_set_pixel(rgb_led, 0, red, green, blue));
	ESP_ERROR_CHECK(led_strip_refresh(rgb_led));
	snprintf(current_color, sizeof(current_color), "%s", color);
	output_red = red;
	output_green = green;
	output_blue = blue;
	xSemaphoreGive(led_mutex);
	oled_show_status();
}

static void set_led_color(const char *color, uint8_t red, uint8_t green, uint8_t blue)
{
	base_red = red;
	base_green = green;
	base_blue = blue;
	apply_led_state(color);
}

static bool parse_hex_color(const char *value, uint8_t *red, uint8_t *green, uint8_t *blue)
{
	if (strlen(value) != 6) {
		return false;
	}
	unsigned int parsed_red;
	unsigned int parsed_green;
	unsigned int parsed_blue;
	if (sscanf(value, "%2x%2x%2x", &parsed_red, &parsed_green, &parsed_blue) != 3) {
		return false;
	}
	*red = (uint8_t)parsed_red;
	*green = (uint8_t)parsed_green;
	*blue = (uint8_t)parsed_blue;
	return true;
}

static esp_err_t root_handler(httpd_req_t *request)
{
	static const char page[] =
		"<!doctype html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\"><title>ESP32-S3 LED</title>"
		"<style>body{font-family:system-ui;text-align:center;margin:2rem;background:#101820;color:#fff}button{font-size:1.2rem;margin:.4rem;padding:.8rem 1.5rem;border:0;border-radius:8px;cursor:pointer}"
		"input{margin:1rem;width:min(90vw,360px)}.red{background:#e53935}.green{background:#43a047}.blue{background:#1e88e5}.white{background:#eee}.off{background:#555;color:#fff}</style></head>"
		"<body><h1>ESP32-S3 RGB LED</h1><button class=red onclick=\"setColor('red')\">Red</button>"
		"<button class=green onclick=\"setColor('green')\">Green</button><button class=blue onclick=\"setColor('blue')\">Blue</button>"
		"<button class=white onclick=\"setColor('white')\">White</button><button class=off onclick=\"setColor('off')\">Off</button>"
		"<p>Custom color</p><input id=color type=color value=\"#ffffff\" oninput=\"setCustomColor()\"><p>Brightness: <span id=value>255</span></p>"
		"<input id=brightness type=range min=0 max=255 value=255 oninput=\"setBrightness(this.value)\"><p id=status>Ready</p>"
		"<script>function update(query){fetch('/set?'+query).then(r=>r.text()).then(t=>status.textContent=t)}"
		"function setColor(c){update('color='+c)}function setCustomColor(){update('color='+color.value.substring(1))}"
		"function setBrightness(v){value.textContent=v;update('brightness='+v)}</script></body></html>";
	httpd_resp_set_type(request, "text/html");
	return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t set_handler(httpd_req_t *request)
{
	char query[64] = {0};
	char color[12] = {0};
	char brightness_text[4] = {0};
	bool changed = false;
	if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) {
		return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Missing parameters");
	}
	if (httpd_query_key_value(query, "brightness", brightness_text, sizeof(brightness_text)) == ESP_OK) {
		int requested_brightness = atoi(brightness_text);
		if (requested_brightness < 0 || requested_brightness > 255) {
			return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Brightness must be 0-255");
		}
		brightness = (uint8_t)requested_brightness;
		changed = true;
	}
	if (httpd_query_key_value(query, "color", color, sizeof(color)) == ESP_OK) {
		uint8_t red;
		uint8_t green;
		uint8_t blue;
		if (strcmp(color, "red") == 0) {
			set_led_color("RED", 255, 0, 0);
		} else if (strcmp(color, "green") == 0) {
			set_led_color("GREEN", 0, 255, 0);
		} else if (strcmp(color, "blue") == 0) {
			set_led_color("BLUE", 0, 0, 255);
		} else if (strcmp(color, "white") == 0) {
			set_led_color("WHITE", 255, 255, 255);
		} else if (strcmp(color, "off") == 0) {
			set_led_color("OFF", 0, 0, 0);
		} else if (parse_hex_color(color, &red, &green, &blue)) {
			set_led_color("CUSTOM", red, green, blue);
		} else {
			return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Unknown color");
		}
		changed = true;
	}
	if (httpd_query_key_value(query, "brightness", brightness_text, sizeof(brightness_text)) == ESP_OK) {
		apply_led_state(current_color);
	}
	if (!changed) {
		return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Missing parameters");
	}
	return httpd_resp_sendstr(request, current_color);
}

static void start_web_server(void)
{
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	httpd_handle_t server = NULL;
	ESP_ERROR_CHECK(httpd_start(&server, &config));
	httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_handler};
	httpd_uri_t set = {.uri = "/set", .method = HTTP_GET, .handler = set_handler};
	ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
	ESP_ERROR_CHECK(httpd_register_uri_handler(server, &set));
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		wifi_connected = false;
		snprintf(current_ip, sizeof(current_ip), "0.0.0.0");
		esp_wifi_connect();
		oled_show_status();
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		const ip_event_got_ip_t *event = event_data;
		snprintf(current_ip, sizeof(current_ip), IPSTR, IP2STR(&event->ip_info.ip));
		wifi_connected = true;
		ESP_LOGI(TAG, "Wi-Fi connected, IP address: %s", current_ip);
		oled_show_status();
	}
}

static void wifi_init(void)
{
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();
	wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
	wifi_config_t station_config = {
		.sta = {.ssid = CONFIG_WIFI_SSID, .password = CONFIG_WIFI_PASSWORD,
					.threshold.authmode = WIFI_AUTH_OPEN, .pmf_cfg = {.capable = true, .required = false}},
	};
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station_config));
	ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void)
{
	esp_err_t nvs_result = nvs_flash_init();
	if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		nvs_result = nvs_flash_init();
	}
	ESP_ERROR_CHECK(nvs_result);

	led_mutex = xSemaphoreCreateMutex();
	oled_init();
	oled_show_status();

	led_strip_config_t strip_config = {
		.strip_gpio_num = RGB_LED_GPIO,
		.max_leds = 1,
		.led_model = LED_MODEL_WS2812,
		.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
		.flags.invert_out = false,
	};
	led_strip_rmt_config_t rmt_config = {
		.clk_src = RMT_CLK_SRC_DEFAULT,
		.resolution_hz = 10 * 1000 * 1000,
		.mem_block_symbols = 64,
		.flags.with_dma = false,
	};
	ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &rgb_led));
	set_led_color("OFF", 0, 0, 0);
	wifi_init();
	start_web_server();
	ESP_LOGI(TAG, "Web server started");
}
