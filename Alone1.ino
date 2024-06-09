#include <stdlib.h>
#include <vector>
#include <deque>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <Arduino.h>
#include <esp_pthread.h>
#include <WiFi.h>
// #include <DNSServer.h>
#include <SSLCert.hpp>
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <HTTPURLEncodedBodyParser.hpp>
#include <HTTPSServer.hpp>
#include <SD.h>
#include <RTClib.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/TomThumb.h>
#include <Adafruit_BME280.h>

#include "config.h"

static std::mutex mutex_1;
#define DISPLAY_LOCK(lock) std::lock_guard<std::mutex> lock(mutex_1);
#define DEVICE_LOCK(lock) std::lock_guard<std::mutex> lock(mutex_1);
#define SDCARD_LOCK(lock)
#define NETWORK_LOCK(lock)

static std::mutex wait_measure_mutex;
static std::condition_variable wait_measure_condition;

static bool need_save = false;
static bool need_reboot = false;

static Adafruit_SSD1306 Monitor(128, 64);

/*****************************************************************************/
/* Data */

struct Data {
	DateTime time;
	float temperature;
	float pressure;
	float humidity;
};

static char const *data_fields[] = {
	"time",
	"temperature",
	"pressure",
	"humidity"
};

static String CSV_Data(struct Data const *const data) {
	return show_time(&data->time) + ',' + data->temperature + ',' + data->pressure + ',' + data->humidity;
}

static String pretty_Data(struct Data const *const data) {
	char date[11], time[9];
	String fulltime = show_time(&data->time);
	if (fulltime.length() ==19) {
		memcpy(date, fulltime.c_str(), 10);
		date[10] = 0;
		memcpy(time, fulltime.c_str() + 11, 8);
		time[8] = 0;
	}
	else {
		date[0] = '?';
		date[1] = 0;
		time[0] = '?';
		time[1] = 0;
	}
	return String("Time:\r\n") + date + "\r\n" + time + "\r\n"
		+ "Temperature:\r\n" + String(data->temperature) + "\r\n"
		+ "Pressure:\r\n" + String(data->pressure) + "\r\n"
		+ "Humidity:\r\n" + String(data->humidity) + "\r\n";
}

/*****************************************************************************/
/* SD card */

static char const setting_filename[] = "/setting.txt";
static char const data_filename[] = "/data.csv";
static String data_header;

// static SPIClass SPI_1(HSPI);
static bool has_SD_card;

static void save_settings(void) {
	if (!has_SD_card) return;
	SDCARD_LOCK(sdcard_lock)
	File file = SD.open(setting_filename, "w", true);
	if (!file) {
		Serial.println("ERROR: failed to open setting file");
		return;
	}
	file.println(device_name);
	file.println(measure_interval / 1000);
	file.println(int(use_AP_mode));
	file.println(AP_SSID);
	file.println(AP_PASS);
	file.println(STA_SSID);
	file.println(STA_PASS);
	file.println(report_URL);
	file.close();
}

static bool load_settings(void) {
	char *e;
	String s;
	unsigned long int u;

	if (digitalRead(reset_pin) == HIGH) {
		Serial.println("Setting is not loaded because of hardware switch");
		return false;
	}
	SDCARD_LOCK(sdcard_lock)
	File file = SD.open(setting_filename, "r", false);
	if (!file) {
		Serial.println("Failed to open setting file");
		return false;
	}

	device_name = file.readStringUntil('\n');
	device_name.trim();
	s = file.readStringUntil('\n');
	s.trim();
	u = strtoul(s.c_str(), &e, 10);
	if (!*e && u >= 15 && u <= 900) measure_interval = u * 1000;
	s = file.readStringUntil('\n');
	s.trim();
	u = strtoul(s.c_str(), &e, 10);
	if (!*e) use_AP_mode = bool(u);
	AP_SSID = file.readStringUntil('\n');
	AP_SSID.trim();
	AP_PASS = file.readStringUntil('\n');
	AP_PASS.trim();
	STA_SSID = file.readStringUntil('\n');
	STA_SSID.trim();
	STA_PASS = file.readStringUntil('\n');
	STA_PASS.trim();
	report_URL = file.readStringUntil('\n');
	report_URL.trim();

	file.close();
	return true;
}

/*****************************************************************************/
/* Real-time clock */

static RTC_Millis internal_clock;
static bool internal_clock_available = false;
static RTC_DS3231 external_clock;
static bool external_clock_available = false;

static bool clock_available(void) {
	return external_clock_available || internal_clock_available;
}

static void set_time(DateTime const datetime) {
	if (external_clock_available) {
		DEVICE_LOCK(device_lock)
		external_clock.adjust(datetime);
	}
	else {
		internal_clock.adjust(datetime);
		internal_clock_available = true;
	}
}

static DateTime get_time(void) {
	if (external_clock_available) {
		DEVICE_LOCK(device_lock)
		return external_clock.now();
	}
	else
		return internal_clock.now();
}

static String show_time(DateTime const *const datetime) {
	if (datetime->isValid())
		return datetime->timestamp();
	else
		return String("?");
}

/*****************************************************************************/
/* Measurement */

Adafruit_BME280 BME280;

static size_t const records_max_size = 60;
static std::deque<Data> records;

static void measure(void) {
	Data data;
	if (clock_available())
		data.time = get_time();
	else
		data.time = DateTime(0, 0, 0);
	DEVICE_LOCK(device_lock)
	data.temperature = BME280.readTemperature();
	data.pressure = BME280.readPressure();
	data.humidity = BME280.readHumidity();

	String const data_string = CSV_Data(&data);
	Serial.print("Measure ");
	Serial.println(data_string);

	if (records.size() >= records_max_size) records.pop_back();
	records.push_front(data);

	if (has_SD_card) {
		File file = SD.open(data_filename, "a", true);
		try {
			file.println(data_string);
		}
		catch (...) {
			Serial.println("ERROR: failed to write data into SD card");
		}
		file.close();
	}

	redraw_display();
}

static void measure_thread(void) {
	for (;;)
		try {
			measure();
			std::unique_lock<std::mutex> wait_lock(wait_measure_mutex);
			// delay(measure_interval);
			//	std::this_thread::sleep_for(std::chrono::duration<unsigned long int, std::milli>(measure_interval));
			wait_measure_condition.wait_for(wait_lock, std::chrono::duration<unsigned long int, std::milli>(measure_interval));
		}
		catch (...) {
			Serial.println("ERROR: exception in measurement");
		}
}

/*****************************************************************************/
/* WiFi */

// static DNSServer DNSd;

static void handle_WiFi_event(WiFiEvent_t const event) {
	switch (event) {
	case ARDUINO_EVENT_WIFI_READY:
		Serial.println("WiFi interface ready");
		break;
	case ARDUINO_EVENT_WIFI_SCAN_DONE:
		Serial.println("Completed scan for access points");
		break;
	case ARDUINO_EVENT_WIFI_STA_START:
		Serial.println("WiFi client started");
		break;
	case ARDUINO_EVENT_WIFI_STA_STOP:
		Serial.println("WiFi clients stopped");
		break;
	case ARDUINO_EVENT_WIFI_STA_CONNECTED:
		Serial.println("Connected to access point");
		break;
	case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
		Serial.println("Disconnected from WiFi access point");
		break;
	case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
		Serial.println("Authentication mode of access point has changed");
		break;
	case ARDUINO_EVENT_WIFI_STA_GOT_IP:
		Serial.print("Obtained IP address: ");
		Serial.println(WiFi.localIP());
		break;
	case ARDUINO_EVENT_WIFI_STA_LOST_IP:
		Serial.println("Lost IP address and IP address is reset to 0");
		break;
	case ARDUINO_EVENT_WPS_ER_SUCCESS:
		Serial.println("WiFi Protected Setup (WPS): succeeded in enrollee mode");
		break;
	case ARDUINO_EVENT_WPS_ER_FAILED:
		Serial.println("WiFi Protected Setup (WPS): failed in enrollee mode");
		break;
	case ARDUINO_EVENT_WPS_ER_TIMEOUT:
		Serial.println("WiFi Protected Setup (WPS): timeout in enrollee mode");
		break;
	case ARDUINO_EVENT_WPS_ER_PIN:
		Serial.println("WiFi Protected Setup (WPS): pin code in enrollee mode");
		break;
	case ARDUINO_EVENT_WIFI_AP_START:
		Serial.println("WiFi access point started");
		break;
	case ARDUINO_EVENT_WIFI_AP_STOP:
		Serial.println("WiFi access point  stopped");
		break;
	case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
		Serial.println("Client connected");
		break;
	case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
		Serial.println("Client disconnected");
		break;
	case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
		Serial.println("Assigned IP address to client");
		break;
	case ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED:
		Serial.println("Received probe request");
		break;
	case ARDUINO_EVENT_WIFI_AP_GOT_IP6:
		Serial.println("AP IPv6 is preferred");
		break;
	case ARDUINO_EVENT_WIFI_STA_GOT_IP6:
		Serial.println("STA IPv6 is preferred");
		break;
	case ARDUINO_EVENT_ETH_GOT_IP6:
		Serial.println("Ethernet IPv6 is preferred");
		break;
	case ARDUINO_EVENT_ETH_START:
		Serial.println("Ethernet started");
		break;
	case ARDUINO_EVENT_ETH_STOP:
		Serial.println("Ethernet stopped");
		break;
	case ARDUINO_EVENT_ETH_CONNECTED:
		Serial.println("Ethernet connected");
		break;
	case ARDUINO_EVENT_ETH_DISCONNECTED:
		Serial.println("Ethernet disconnected");
		break;
	case ARDUINO_EVENT_ETH_GOT_IP:
		Serial.println("Obtained IP address");
		break;
	default:
		Serial.print("Unknown WiFi event ");
		Serial.println(event);
		break;
	}
}

static char const *status_message(signed int const WiFi_status) {
	switch (WiFi_status) {
	case WL_NO_SHIELD:
		return "WiFi no shield";
	case WL_IDLE_STATUS:
		return "WiFi idle";
	case WL_NO_SSID_AVAIL:
		return "WiFi no SSID";
	case WL_SCAN_COMPLETED:
		return "WiFi scan completed";
	case WL_CONNECTED:
		return "WiFi connected";
	case WL_CONNECT_FAILED:
		return "WiFi connect failed";
	case WL_CONNECTION_LOST:
		return "WiFi connection lost";
	case WL_DISCONNECTED:
		return "WiFi disconnected";
	default:
		return "WiFi Status: " + WiFi_status;
	}
}

static signed int check_WiFi_status(void) {
	static signed int last_status = WL_NO_SHIELD;
	signed int status = WiFi.status();
	if (status != last_status) {
		last_status = status;
		Serial.println(status_message(status));
		if (status == WL_CONNECTED) {
			String const SSID = WiFi.SSID();
			Serial.print("WiFi SSID: ");
			Serial.println(WiFi.SSID());
			Serial.print("IP address: ");
			Serial.println(WiFi.localIP().toString());
		}
		redraw_display();
	}
	return status;
}

static void wifi_thread(void) {
	for (;;)
		try {
			delay(WiFi_check_interval);
			check_WiFi_status();
		}
		catch (...) {
			Serial.println("ERROR: exception in WiFi checking");
		}
}

static void setup_WiFi(void) {
	WiFi.disconnect();
	WiFi.onEvent(handle_WiFi_event);

	if (use_AP_mode) {
		/* WiFi access-point */
		WiFi.disconnect();
		WiFi.mode(WIFI_AP);
		WiFi.setHostname("WeatherStation");
		// IPAddress my_IP_address = IPAddress(8, 8, 8, 8);
		// WiFi.softAPConfig(my_IP_address, my_IP_address, IPAddress(255, 255, 255, 0));
		while (!WiFi.softAP(AP_SSID.c_str(), AP_PASS, 1, 0, 2)) {
			Serial.println("ERROR: failed to create soft AP");
			Monitor.println("ERROR: WiFi AP");
			Monitor.display();
			delay(reinitialize_interval);
		}
		Serial.print("WiFi SSID: ");
		Serial.println(WiFi.softAPSSID());
		Serial.print("IP address: ");
		Serial.println(WiFi.softAPIP().toString());

		/* DNS server */
		// static uint16_t const DNS_port = 53;
		// static String const DNS_domain("*");
		// while (!DNSd.start(DNS_port, DNS_domain, my_IP_address)) {
		// 	Serial.println("ERROR: failed to create DNS server");
		// 	Monitor.println("ERROR: DNS server");
		// 	delay(reinitialize_interval);
		// }
	}
	else {
		/* WiFi stationary */
		WiFi.mode(WIFI_STA);
		WiFi.begin(STA_SSID, STA_PASS);
		while (WiFi.status() == WL_NO_SHIELD) {
			Serial.println("ERROR: no WiFi shield");
			Monitor.println("ERROR: WiFi shield");
			Monitor.display();
			delay(reinitialize_interval);
		}
		while (millis() < WiFi_wait_time && WiFi.status() != WL_CONNECTED)
			delay(1);
		std::thread(wifi_thread).detach();
	}
}

/*****************************************************************************/
/* Web server */

static httpsserver::HTTPServer HTTPd;
static httpsserver::SSLCert certificate;
static httpsserver::HTTPSServer HTTPSd(&certificate);

static PROGMEM char const web_home_html_1[] =
R"HTML(<html xmlns='http://www.w3.org/1999/xhtml'>
<head>
<meta content-type='application/xhtml+xml; charset=UTF-8' />
<meta charset='UTF-8' />
<meta name='viewport' content='width=device-width, initial-scale=1' />
<title>Weather data</title>
<link rel='stylesheet' type='text/css' href='style.css' />
</head>
<body>
<noscript>Javascript is required for this web page.</noscript>
<script type='text/javascript'>
	(function(p){document.readyState!=='loading'?p():document.addEventListener('DOMContentLoaded',p)})
	(function(p){window.Alone={

)HTML";

static PROGMEM char const web_home_html_2[] = R"HTML(

	};return import('./script.js').then(function(){},p);}
	(function(SD_load_error){
		'use strict';
		console.log('Failed to load script from SD card:', SD_load_error);
		document.body.textContent = '';
		function $T(string) {
			return document.createTextNode(string);
		}
		function $E(name) {
			return document.createElementNS(document.documentElement.namespaceURI, name);
		}
		function c_(parent, child) {
			return parent.appendChild(child);
		}
		function s_(element, name, value) {
			return element.style[name] = value;
		}
		function a_(element, name, value) {
			return element.setAttribute(name, value);
		}
		function string_from_Date(value, seperator = ' ') {
			var date = new Date(value);
			return (
				date.getFullYear().toString()
					+ '-'
					+ (date.getMonth() + 1).toString().padStart(2, '0')
					+ '-'
					+ date.getDate().toString().padStart(2, '0')
					+ seperator
					+ date.getHours().toString().padStart(2, '0')
					+ ':'
					+ date.getMinutes().toString().padStart(2, '0')
					+ ':'
					+ date.getSeconds().toString().padStart(2, '0')
			);
		}
		var $save_GPS;
		void function () {
			var $p, $a;
			function style_$a() {
				s_($a, 'margin', '1ex');
				s_($a, 'border', 'solid thin gray');
				s_($a, 'padding', '1ex');
			}
			$p = $E('p');
			s_($p, 'display', 'flex');
			s_($p, 'flex-flow', 'row wrap');
			s_($p, 'text-align', 'center');
			$a = $E('a');
			style_$a();
			a_($a, 'href', 'setting.html');
			c_($a, $T('Settings'));
			c_($p, $a);
			$a = $E('a');
			style_$a();
			a_($a, 'href', 'recent.csv');
			a_($a, 'download', '');
			c_($a, $T('Download recent data'));
			c_($p, $a);
			$a = $E('a');
			style_$a();
			a_($a, 'href', 'data.csv');
			a_($a, 'download', '');
			c_($a, $T('Download all data'));
			c_($p, $a);
			$save_GPS = $a = $E('a');
			style_$a();
			a_($a, 'href', '.');
			c_($a, $T('Save GPS data'));
			c_($p, $a);
			c_(document.body, $p);
		}();
		var $refresh, $refresh_auto;
		void function () {
			var $form, $button, $label, $input;
			$refresh = $form = $E('form');
			s_($form, 'display', 'inline-block');
			s_($form, 'margin', '1ex');
			s_($form, 'border', 'solid thin gray');
			s_($form, 'padding', '1ex');
			$label = $E('label');
			s_($label, 'margin-right', '1ex');
			s_($label, 'padding', '1ex');
			$refresh_auto = $input = $E('input');
			a_($input, 'type', 'checkbox');
			c_($label, $input);
			c_($label, $T('Auto refresh'));
			c_($form, $label);
			$button = $E('button');
			a_($button, 'type', 'submit');
			s_($button, 'margin-left', '1ex');
			c_($button, $T('Refresh now'));
			c_($form, $button);
			c_(document.body, $form);
		}();
		var $report, $report_auto;
		void function () {
			var $form, $button, $label, $input;
			$report = $form = $E('form');
			s_($form, 'display', 'inline-block');
			s_($form, 'margin', '1ex');
			s_($form, 'border', 'solid thin gray');
			s_($form, 'padding', '1ex');
			$label = $E('label');
			s_($label, 'margin-right', '1ex');
			s_($label, 'padding', '1ex');
			$report_auto = $input = $E('input');
			a_($input, 'type', 'checkbox');
			c_($label, $input);
			c_($label, $T('Report to monitor'));
			c_($form, $label);
			$button = $E('button');
			a_($button, 'type', 'submit');
			s_($button, 'margin-left', '1ex');
			c_($button, $T('Report now'));
			c_($form, $button);
			c_(document.body, $form);
		}();
		var latest = null;
		var $list;
		void function () {
			var $table, $caption, $thead, $tr, $th, $tbody;
			$table = $E('table');
			s_($table, 'margin-bottom', '3ex');
			s_($table, 'border-collapse', 'collapse');
			s_($table, 'width', '100%');
			$caption = $E('caption');
			c_($caption, $T('Sensor data'));
			c_($table, $caption);
			$thead = $E('thead');
			s_($thead, 'border-bottom-style', 'solid');
			$tr = $E('tr');
			Alone.data_fields.forEach(
				function (field) {
					$th = $E('th');
					c_($th, $T(field[0].toUpperCase() + field.substring(1)));
					c_($tr, $th);
				}
			);
			c_($thead, $tr);
			c_($table, $thead);
			$list = $tbody = $E('tbody');
			c_($table, $tbody);
			c_(document.body, $table);
		}();
		function load() {
			$list.textContent = null;
			var $loading = $E('p');
			c_($loading, $T('Loading...'));
			c_(document.body, $loading);
			var xhr = new XMLHttpRequest();
			xhr.onloadend = function (event) {
				document.body.removeChild($loading);
				var text = xhr.responseText;
				if (text == null || xhr.status !== 200) {
					alert('Failed to load data');
					return;
				}
				var fields = null;
				var lines = text.split('\r\n');
				if (!lines || !(lines.length > 0)) return;
				for (var i = 1; lines.length > i; ++i) {
					var line = lines[lines.length - i].trim();
					if (!line || typeof line !== 'string') continue;
					fields = line.split(',');
					var $tr = $E('tr');
					for (var j = 0; fields.length > j; ++j) {
						var $td = $E('td');
						s_($td, 'border-style', 'solid');
						s_($td, 'border-width', 'thin');
						s_($td, 'text-align', 'center');
						if (j === 0) c_($td, $T(string_from_Date(fields[j])));
						else c_($td, $T(fields[j]));
						c_($tr, $td);
					}
					c_($list, $tr);
				}
				latest = fields;
			};
			xhr.open('GET', '/recent.csv', true);
			xhr.send(null);
		}
		function report() {
			if (!GPS.length) return;
			var position = GPS[GPS.length - 1];
			var formdata = new FormData;
			formdata.append('identity',  Alone.identity);
			formdata.append('time',      position[0]);
			formdata.append('latitude',  position[1]);
			formdata.append('longitude', position[2]);
			formdata.append('latitude',  position[3]);
			if (Array.isArray(latest))
				for (var i = 1; Alone.data_fields.length > i; ++i)
					formdata.append(Alone.data_fields[i], latest[i]);
			fetch(Alone.report, {method: 'POST', body: formdata})
				.catch(function () {});
		}
		$refresh.addEventListener(
			'submit',
			function (event) {
				event.preventDefault();
				load();
			}
		);
		var refresh_timer = null;
		$refresh_auto.addEventListener(
			'change',
			function (event) {
				if ($refresh_auto.checked) {
					if (refresh_timer !== null) return;
					refresh_timer = setInterval(load, 30000);
				}
				else {
					if (refresh_timer === null) return;
					clearInterval(refresh_timer);
					refresh_timer = null;
				}
			}
		);
		$report.addEventListener(
			'submit',
			function (event) {
				event.preventDefault();
				report();
			}
		);
		var report_timer = null;
		$report_auto.addEventListener(
			'change',
			function (event) {
				if ($report.checked) {
					if (report_timer !== null) return;
					report_timer = setInterval(report, 30000);
				}
				else {
					if (report_timer === null) return;
					clearInterval(report_timer);
					report_timer = null;
				}
			}
		);
		load();
		var GPS = new Array;
		if ('geolocation' in navigator)
			if (window.isSecureContext) {
				var $GPS;
				void function () {
					var $table, $caption, $thead, $tr, $th, $tbody;
					$table = $E('table');
					s_($table, 'border-collapse', 'collapse');
					s_($table, 'width', '100%');
					$caption = $E('caption');
					c_($caption, $T('GPS data'));
					c_($table, $caption);
					$thead = $E('thead');
					s_($thead, 'border-bottom-style', 'solid');
					$tr = $E('tr');
					['Time', 'Latitude', 'Longitude', 'Altitude'].forEach(
						function (title) {
							$th = $E('th');
							c_($th, $T(title));
							c_($tr, $th);
						}
					);
					c_($thead, $tr);
					c_($table, $thead);
					$GPS = $tbody = $E('tbody');
					c_($table, $tbody);
					c_(document.body, $table);
				}();
				function record_GPS(spacetime) {
					var coords = spacetime.coords;
					GPS.push(
						[
							string_from_Date(spacetime.timestamp, 'T'),
							coords.latitude, coords.longitude, coords.altitude,
							coords.accuracy, coords.altitudeAccuracy,
							coords.heading, coords.speed
						]
					);
					var $tr = $E('tr');
					function add_td(value) {
						var $td = $E('td');
						s_($td, 'border-style', 'solid');
						s_($td, 'border-width', 'thin');
						s_($td, 'text-align', 'center');
						if (value != null) c_($td, $T(String(value)));
						c_($tr, $td);
					}
					add_td(string_from_Date(spacetime.timestamp));
					add_td(coords.latitude);
					add_td(coords.longitude);
					add_td(coords.altitude);
					c_($GPS, $tr);
				}
				function get_GPS() {
					navigator.geolocation.getCurrentPosition(
						record_GPS,
						function (error) {
							console.error("GeoLocationError: ", error.message);
						},
						{timeout: 15000, enableHighAccuracy: true}
					)
				}
				get_GPS();
				setInterval(get_GPS, 30000);
			}
		var $GPS_downloader = $E('a');
		a_($GPS_downloader, 'download', 'gps.csv');
		$GPS_downloader.hidden = true;
		document.body.appendChild($GPS_downloader);
		function save_GPS() {
			var content =
				'Time,Latitude,Longitude,Altitude,Horizontal accuracy,Vertical accuracy,Heading,Speed\r\n'
					+ GPS.map(function (record) {return record.join(',');}).join('\r\n');
			var oldobj = $GPS_downloader.getAttribute('href');
			if (oldobj) URL.revokeObjectURL(objurl);
			$GPS_downloader.setAttribute('href', URL.createObjectURL(new Blob(Array.of(content), {type: 'text/csv'})));
			setTimeout(function () {$GPS_downloader.click();}, 1000);
		}
		$GPS_downloader.addEventListener(
			'click',
			function (event) {
				event.preventDefault();
				save_GPS();
			}
		);
	}));
</script>
</body>
</html>
)HTML";

static String javascript_escape(String const &string) {
	String result;
	for (char const c: string)
		switch (c) {
		case '\"':
			result.concat("\\\"");
			break;
		case '\'':
			result.concat("\\\'");
			break;
		default:
			result.concat(c);
		}
	return result;
}

static void web_home_handle(httpsserver::HTTPRequest *const request, httpsserver::HTTPResponse *const response) {
	response->setHeader("CONTENT-TYPE", "application/xhtml+xml; charset=UTF-8");
	response->setHeader("CONTENT-SECURITY-POLICY", "connect-src *");
	response->write(reinterpret_cast<byte const *>(web_home_html_1), sizeof web_home_html_1 - 1);
	response->print("\t\tidentity: '");
	response->print(javascript_escape(device_name));
	response->print("',\r\n\t\treport: '");
	response->print(javascript_escape(report_URL));
	response->print("',\r\n\t\tdata_fields: [");
	bool first = true;
	for (char const *field: data_fields) {
		if (first)
			first = false;
		else
			response->print(", ");
		response->print('\'');
		response->print(javascript_escape(field));
		response->print('\'');
	}
	response->print("]\r\n");
	response->write(reinterpret_cast<byte const *>(web_home_html_2), sizeof web_home_html_2 - 1);
	response->finalize();
}

static httpsserver::ResourceNode web_home_node("/", "GET", web_home_handle);

static PROGMEM char const web_icon_data[] = {
	/* PNG signature */
	0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
	/* data length */
	0x00, 0x00, 0x00, 0x0D,
	/* "IHDR" as ASCII */
	0x49, 0x48, 0x44, 0x52,
	/* width */
	0x00, 0x00, 0x00, 0x01,
	/* height */
	0x00, 0x00, 0x00, 0x01,
	/* bit depth */
	0x01,
	/* colour type */
	0x00,
	/* compression method */
	0x00,
	/* filter method */
	0x00,
	/* interlace method */
	0x00,
	/* checksum */
	0x37, 0x6E, 0xF9, 0x24
};

static void web_icon_handle(httpsserver::HTTPRequest *const request, httpsserver::HTTPResponse *const response) {
	response->setHeader("CONTENT-TYPE", "image/png");
	response->write(reinterpret_cast<byte const *>(web_icon_data), sizeof web_icon_data - 1);
	response->finalize();
}

static httpsserver::ResourceNode web_icon_node("/favicon.ico", "GET", web_icon_handle);

static void web_data_handle(httpsserver::HTTPRequest *const request, httpsserver::HTTPResponse *const response) {
	response->setHeader("CONTENT-TYPE", "text/csv");
	response->println(data_header);
	for (Data const &record: records)
		response->println(CSV_Data(&record));
}

static httpsserver::ResourceNode web_data_node("/recent.csv", "GET", web_data_handle);
static char buffer[32768];

static PROGMEM char const web_setting_html_1[] =
R"HTML(<html xmlns='http://www.w3.org/1999/xhtml'>
<head>
<meta content-type='application/xhtml+xml; charset=UTF-8' />
<meta charset='UTF-8' />
<meta name='viewport' content='width=device-width, initial-scale=1' />
<title>Settings</title>
<link rel='icon' type='image/png' href='favicon.ico' />
<link rel='stylesheet' type='text/css' href='style.css' />
</head>
<body>
<p><a href='./'>&#x2190; Back</a></p>
)HTML";

static PROGMEM char const web_setting_html_2[] = R"HTML(
</body>
</html>
)HTML";

static PROGMEM char const web_setting_form_1[] = "<form\r\n\tid='";

static PROGMEM char const web_setting_form_2[] = R"HTML('
	action='setting.exe'
	method='POST'
	style='margin: 1ex; border: solid thin; padding: 1ex'
>
)HTML";

static String XML_escape(String const &string) {
	String result;
	for (char const c: string)
		switch (c) {
		case '&':
			result.concat("&amp;");
			break;
		case '<':
			result.concat("&lt;");
			break;
		case '>':
			result.concat("&gt;");
			break;
		case '"':
			result.concat("&quot;");
			break;
		case '\'':
			result.concat("&apos;");
			break;
		default:
			result.concat(c);
		}
	return result;
}

static void web_setting_form(httpsserver::HTTPResponse *const response, char const *const id) {
	response->write(reinterpret_cast<byte const *>(web_setting_form_1), sizeof web_setting_form_1 - 1);
	response->print(XML_escape(id));
	response->write(reinterpret_cast<byte const *>(web_setting_form_2), sizeof web_setting_form_2 - 1);
}

static void web_setting_handle(httpsserver::HTTPRequest *const request, httpsserver::HTTPResponse *const response) {
	response->setHeader("CONTENT-TYPE", "application/xhtml+xml; charset=UTF-8");
	response->write(reinterpret_cast<byte const *>(web_setting_html_1), sizeof web_setting_html_1 - 1);

	web_setting_form(response, "set_time");
	response->print(
		"\t<label>\r\n"
		"\t\tCurrent time \r\n"
		"\t\t<input type='datetime-local' name='time' required='' />\r\n"
		"\t</label>\r\n"
		"\t<button type='submit'>Set</button>\r\n"
		"</form>\r\n"
	);

	web_setting_form(response, "set_name");
	response->print(
		"\t<label>\r\n"
		"\t\tDevice ID\r\n"
		"\t\t<input type='text' name='name' required='' value='"
	);
	response->print(XML_escape(device_name));
	response->print(
		"' />\r\n"
		"\t</label>\r\n"
		"\t<button type='submit'>Set</button>\r\n"
		"</form>\r\n"
	);

	web_setting_form(response, "set_interval");
	response->print(
		"\t<label>\r\n"
		"\t\tMeasure interval / seconds\r\n"
		"\t\t<input type='number' name='interval' min='10' max='900' required='' value='"
	);
	response->print(String(measure_interval / 1000));
	response->print(
		"' />"
		"\t</label>\r\n"
		"\t<button type='submit'>Set</button>\r\n"
		"</form>\r\n"
	);

	web_setting_form(response, "set_wifi");
	response->print(
		"\t<label style='display: block'>\r\n"
		"\t\tProvide WiFi\r\n"
		"\t\t<select name='WiFi'>\r\n"
		"\t\t\t<option value='AP'"
	);
	if (use_AP_mode) response->print(" selected=''");
	response->print(
		">\r\n"
		"\t\t\t\tAccess point\r\n"
		"\t\t\t</option>\r\n"
		"\t\t\t<option value='STA'"
	);
	if (!use_AP_mode) response->print(" selected=''");
	response->print(
		">\r\n"
		"\t\t\t\tStation\r\n"
		"\t\t\t</option>\r\n"
		"\t\t</select>\r\n"
		"\t</label>\r\n"
		"\t<label style='display: block'>\r\n"
		"\t\tAP SSID\r\n"
		"\t\t<input name='APSSID' value='"
	);
	response->print(XML_escape(AP_SSID));
	response->print(
		"' />\r\n"
		"\t</label>\r\n"
		"\t<label style='display: block'>\r\n"
		"\t\tAP PASS\r\n"
		"\t\t<input name='APPASS' value='"
	);
	response->print(XML_escape(AP_PASS));
	response->print(
		"' />\r\n"
		"\t</label>\r\n"
		"\t<label style='display: block'>\r\n"
		"\t\tSTA SSID\r\n"
		"\t\t<input name='STASSID' value='"
	);
	response->print(XML_escape(STA_SSID));
	response->print(
		"' />\r\n"
		"\t</label>\r\n"
		"\t<label style='display: block'>\r\n"
		"\t\tSTA PASS\r\n"
		"\t\t<input name='STAPASS' value='"
	);
	response->print(XML_escape(STA_PASS));
	response->print(
		"' />\r\n"
		"\t</label>\r\n"
		"\t<button type='submit'>Set</button>\r\n\r\n"
		"</form>\r\n"
	);

	web_setting_form(response, "set_report");
	response->print(
		"\t<label>\r\n"
		"\t\tReport URL\r\n"
		"\t\t<input type='text' name='report' required='' value='"
	);
	response->print(XML_escape(report_URL));
	response->print(
		"' />\r\n"
		"\t</label>\r\n"
		"\t<button type='submit'>Set</button>\r\n"
		"</form>\r\n"
	);

	web_setting_form(response, "do_measure");
	response->print(
		"\t<label style='display: block'>\r\n"
		"\t\tConfirm\r\n"
		"\t\t<input type='checkbox' name='measure' />\r\n"
		"\t</label>\r\n"
		"\t<button type='submit'>Measure now</button>\r\n"
		"</form>\r\n"
	);

	web_setting_form(response, "do_delete");
	response->print(
		"\t<label style='display: block'>\r\n"
		"\t\tConfirm\r\n"
		"\t\t<input type='checkbox' name='delete' />\r\n"
		"\t</label>\r\n"
		"\t<button type='submit'>Delete all data</button>\r\n"
		"</form>\r\n"
	);

	web_setting_form(response, "do_reboot");
	response->print(
		"\t<label style='display: block'>Confirm \r\n"
		"\t\t<input type='checkbox' name='reboot' />\r\n"
		"\t</label>\r\n"
		"\t<button type='submit' name='reboot'>Reboot</button>\r\n"
		"</form>\r\n"
	);

	response->write(reinterpret_cast<byte const *>(web_setting_html_2), sizeof web_setting_html_2 - 1);
	response->finalize();
}

static httpsserver::ResourceNode web_setting_node("/setting.html", "GET", web_setting_handle);

static PROGMEM char const web_command_html[] =
R"HTML(<html xmlns='http://www.w3.org/1999/xhtml'>
<head>
<meta content-type='application/xhtml+xml; charset=UTF-8' />
<meta charset='UTF-8' />
<meta name='viewport' content='width=device-width, initial-scale=1' />
<title>Command redirection</title>
<link rel='stylesheet' type='text/css' href='style.css' />
</head>
<body>
<p>Command received. Redirect to <a href='./setting.html'>homepage.</a></p>
</body>
</html>
)HTML";

static std::string read_parser(httpsserver::HTTPBodyParser &parser) {
	std::string result = "";
	while (!parser.endOfField()) {
		size_t const n = parser.read(reinterpret_cast<byte *>(buffer), sizeof buffer);
		result.append(buffer, n);
	}
	return result;
}

static void web_command_handle(httpsserver::HTTPRequest *const request, httpsserver::HTTPResponse *const response) {
	httpsserver::HTTPURLEncodedBodyParser parser(request);
	while (parser.nextField()) {
		std::string const name = parser.getFieldName();
		if (name == "time") {
			std::string const value = read_parser(parser);
			Serial.print("command time = ");
			Serial.println(value.c_str());
			DateTime const datetime(value.c_str());
			if (datetime.isValid())
				set_time(datetime);
			else {
				Serial.print("WARN: incorrect command time = ");
				Serial.println(value.c_str());
			}
		}
		else if (name == "name") {
			std::string const value = read_parser(parser);
			Serial.print("command name = ");
			Serial.println(value.c_str());
			device_name = value.c_str();
			need_save = true;
		}
		else if (name == "interval") {
			std::string const value = read_parser(parser);
			Serial.print("command interval = ");
			Serial.println(value.c_str());
			char *end;
			unsigned long int const x = strtoul(value.c_str(), &end, 10);
			if (*end == 0 && x >= 15 && x <= 900) {
				measure_interval = x * 1000;
				need_save = true;
			}
			else {
				Serial.print("WARN: incorrect command interval = ");
				Serial.println(value.c_str());
			}
		}
		else if (name == "WiFi") {
			std::string const value = read_parser(parser);
			Serial.print("command WiFi = ");
			Serial.println(value.c_str());
			if (value == "AP") {
				use_AP_mode = true;
				need_save = true;
			}
			else if (value == "STA") {
				use_AP_mode = false;
				need_save = true;
			}
			else {
				Serial.println("WARN: incorrect command WiFi = ");
				Serial.println(value.c_str());
			}
		}
		else if (name == "APSSID") {
			std::string const value = read_parser(parser);
			Serial.print("command APSSID = ");
			Serial.println(value.c_str());
			AP_SSID = value.c_str();
			need_save = true;
		}
		else if (name == "APPASS") {
			std::string const value = read_parser(parser);
			Serial.print("command APPASS = ");
			Serial.println(value.c_str());
			AP_PASS = value.c_str();
			need_save = true;
		}
		else if (name == "STASSID") {
			std::string const value = read_parser(parser);
			Serial.print("command STASSID = ");
			Serial.println(value.c_str());
			STA_SSID = value.c_str();
			need_save = true;
		}
		else if (name == "STAPASS") {
			std::string const value = read_parser(parser);
			Serial.print("command STAPASS = ");
			Serial.println(value.c_str());
			STA_PASS = value.c_str();
			need_save = true;
		}
		else if (name == "report") {
			std::string const value = read_parser(parser);
			Serial.print("command report = ");
			Serial.println(value.c_str());
			report_URL = value.c_str();
			need_save = true;
		}
		else if (name == "measure") {
			Serial.println("command measure");
			wait_measure_condition.notify_all();
		}
		else if (name == "delete") {
			Serial.println("command delete");
			records.clear();
			SDCARD_LOCK(sdcard_lock)
			//	SD.remove(data_filename);
			File file = SD.open(data_filename, "w", true);
			try {
				file.println(data_header);
			}
			catch (...) {
				Serial.println("ERROR: failed to write header into data file");
			}
			file.close();
		}
		else if (name == "reboot") {
			Serial.println("command reboot");
			Serial.flush();
			need_reboot = true;
			need_save = false;
		}
		else {
			Serial.print("WARN: unknown command parameter = ");
			Serial.println(name.c_str());
		}
	}

	response->setStatusCode(303);
	response->setStatusText("SEE OTHER");
	response->setHeader("LOCATION", "/setting.html");
	response->setHeader("CONTENT-TYPE", "application/xhtml+xml; charset=UTF-8");
	response->write(reinterpret_cast<byte const *>(web_command_html), sizeof web_command_html - 1);
	response->finalize();
}

static httpsserver::ResourceNode web_command_node("/setting.exe", "POST", web_command_handle);

static void web_file_handle(httpsserver::HTTPRequest *const request, httpsserver::HTTPResponse *const response) {
	std::string const name = request->getRequestString();
	if (request->getMethod() != "GET") {
		response->setStatusCode(405);
		response->setStatusText("METHOD NOT ALLOWED");
		return;
	}
	File file = SD.open(name.c_str(), "r");
	if (!file) {
		Serial.print("File not found: ");
		Serial.println(name.c_str());
		response->setStatusCode(404);
		response->setStatusText("NOT FOUND");
		response->finalize();
		return;
	}
	try {
		if (name.substr(name.length() - 3) == ".js")
			response->setHeader("CONTENT-TYPE", "text/javascript");
		else if (name.substr(name.length() - 4) == ".css")
			response->setHeader("CONTENT-TYPE", "text/css");
		else if (name.substr(name.length() - 4) == ".csv")
			response->setHeader("CONTENT-TYPE", "text/csv");
		else if (name.substr(name.length() - 4) == ".png")
			response->setHeader("CONTENT-TYPE", "image/png");
		else if (name.substr(name.length() - 4) == ".ico")
			response->setHeader("CONTENT-TYPE", "image/png");
		else
			response->setHeader("CONTENT-TYPE", "application/octet-stream");
		response->setHeader("CACHE-CONTROL", "max-age=604800, immutable");
		while (size_t const n = file.readBytes(buffer, sizeof buffer)) {
			response->write(reinterpret_cast<byte *>(buffer), n);
			yield();
		}
		response->finalize();
	}
	catch (...) {
		Serial.println("ERROR: response file");
	}
	file.close();
}

static httpsserver::ResourceNode web_file_node("", "", web_file_handle);

static void setup_webserver(void) {
	static char const DN[] = "CN=weather.station,O=hku,C=HK";
	while (httpsserver::createSelfSignedCert(certificate, httpsserver::KEYSIZE_2048, DN)) {
		Serial.println("ERROR: failed to create signed certificate");
		delay(reinitialize_interval);
	}
	HTTPd.registerNode(&web_home_node);
	HTTPSd.registerNode(&web_home_node);
	HTTPd.registerNode(&web_icon_node);
	HTTPSd.registerNode(&web_icon_node);
	HTTPd.registerNode(&web_data_node);
	HTTPSd.registerNode(&web_data_node);
	HTTPd.registerNode(&web_setting_node);
	HTTPSd.registerNode(&web_setting_node);
	HTTPd.registerNode(&web_command_node);
	HTTPSd.registerNode(&web_command_node);
	HTTPd.setDefaultNode(&web_file_node);
	HTTPSd.setDefaultNode(&web_file_node);
	for (;;) {
		HTTPd.start();
		HTTPSd.start();
		if (HTTPd.isRunning() && HTTPSd.isRunning()) break;
		Serial.println("ERROR: failed to start HTTPS server");
	}
}

/*****************************************************************************/
/* Main procedures */

static void redraw_display(void) {
	Monitor.clearDisplay();
	Monitor.setCursor(0, 12);
	if (has_SD_card)
		Monitor.println("SD card found");
	else
		Monitor.println("No SD card");
	if (use_AP_mode) {
		Monitor.println("WiFi SSID:");
		Monitor.println(WiFi.softAPSSID());
		Monitor.println("IP address:");
		Monitor.println(WiFi.softAPIP().toString());
	}
	else {
		signed int status = WiFi.status();
		Monitor.println(status_message(status));
		if (WL_CONNECTED) {
			Monitor.println("WiFi SSID:");
			Monitor.println(WiFi.SSID());
			Monitor.println("IP address:");
			Monitor.println(WiFi.localIP().toString());
		}
	}
	if (records.size())
		Monitor.println(pretty_Data(&records.back()));
	Monitor.display();
}

void loop(void) {
	delay(1);
	{
		NETWORK_LOCK(network)
		// if (use_AP_mode) DNSd.processNextRequest();
		HTTPd.loop();
		HTTPSd.loop();
	}

	if (need_save) {
		save_settings();
		need_save = false;
	}

	if (need_reboot) {
		static unsigned long int reboot_time_0 = 0;
		static unsigned long int reboot_time_1 = 0;
		unsigned long int now = millis();
		if (!reboot_time_1) {
			reboot_time_0 = now;
			reboot_time_1 = now + reboot_wait_time;
		}
		else if (
			reboot_time_0 < reboot_time_1 && (now < reboot_time_0 || reboot_time_1 < now) ||
			reboot_time_1 < reboot_time_0 && reboot_time_1 < now && now < reboot_time_0
		) {
			need_reboot = false;
			esp_restart();
		}
	}
}

void setup(void) {
	/* Constants*/
	data_header += data_fields[0];
	for (unsigned int i = 0; i < sizeof data_fields / sizeof *data_fields; ++i) {
		data_header += ',';
		data_header += data_fields[i];
	}

	/* Reset pin */
	pinMode(reset_pin, INPUT_PULLDOWN);

	/* Serial port */
	Serial.begin(serial_baudrate);

	/* OLED display */
	Monitor.begin(SSD1306_SWITCHCAPVCC, 0x3C);
	Monitor.setFont(&TomThumb);
	Monitor.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
	Monitor.setRotation(3);
	Monitor.clearDisplay();
	Monitor.display();
	Monitor.setCursor(0, 0);

	/* Start-up delay */
	delay(start_wait_time);

	/* SD */
	pinMode(SD_MISO, INPUT_PULLUP);
	SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
	has_SD_card = SD.begin(SD_CS, SPI);
	if (has_SD_card)
		load_settings();
	else
		Serial.println("WARN: SD card not found");

	/* Clock */
	external_clock_available = external_clock.begin();

	/* Sensor */
	while (!BME280.begin()) {
		Serial.println("ERROR: BME280 not found");
		delay(reinitialize_interval);
	}

	/* WiFi */
	setup_WiFi();

	/* Web server */
	setup_webserver();

	/* Spawn measurement thread */
	static esp_pthread_cfg_t esp_pthread_cfg = esp_pthread_get_default_config();
	esp_pthread_cfg.stack_size = 4096;
	esp_pthread_cfg.inherit_cfg = true;
	esp_pthread_set_cfg(&esp_pthread_cfg);
	std::thread(measure_thread).detach();
}

/*****************************************************************************/
