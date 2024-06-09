#ifndef INCLUDE_CONFIG_H
#define INCLUDE_CONFIG_H

static String device_name = "CATYXX";
static bool use_AP_mode = true;
static unsigned int measure_interval = 60000;
static String AP_SSID = "CATYXX";
static String AP_PASS = "Password";
static String STA_SSID = "";
static String STA_PASS = "";
static String report_URL = "";

static unsigned int const start_wait_time = 1000;
static unsigned int const reboot_wait_time = 1000;
static unsigned int const WiFi_wait_time = 20000;
static unsigned int const WiFi_check_interval = 10000;
static unsigned int const serial_baudrate = 115200;
static unsigned int const reinitialize_interval = 15000;
static unsigned char const reset_pin = 34;

#endif /* INCLUDE_CONFIG_H */
