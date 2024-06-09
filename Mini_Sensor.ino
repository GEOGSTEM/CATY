#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_SSD1306.h>

#define SEALEVELPRESSURE_HPA (1013.25)

Adafruit_BME280 bme;
Adafruit_SSD1306 SSD1306(128, 64);



void setup(void) {
	pinMode(LED_BUILTIN, OUTPUT);

  SSD1306.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  SSD1306.setRotation(1);
  SSD1306.setTextSize(1);
  SSD1306.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  SSD1306.invertDisplay(false);
  SSD1306.clearDisplay();
  SSD1306.display();
  
  bool status;
  status = bme.begin();  
  if (!status) {
    SSD1306.setCursor(0,10);
    SSD1306.print("BME280 Not Found");
    SSD1306.display();
    while (1);
  }
  
	
  
}

bool light = false;

unsigned int y = 0;

void loop(void) {
//  float bme280_temperature;
//  float bme280_pressure;
//  float bme280_humidity;

	SSD1306.clearDisplay();
	SSD1306.setCursor(5, 5);
  SSD1306.print("CATY Proj");
  SSD1306.setCursor(10, 15);
  SSD1306.print("Seneor 3");
  SSD1306.drawLine(0,25,60,25,WHITE);
  SSD1306.setCursor(0, 35);
  SSD1306.print("Air Temp: ");
  SSD1306.setCursor(8, 45);
	SSD1306.print(bme.readTemperature()+0.4);
  SSD1306.setCursor(46, 45);
  SSD1306.print("C");
  SSD1306.setCursor(0, 65);
  SSD1306.print("Air RH: ");
  SSD1306.setCursor(8, 75);
  SSD1306.print(bme.readHumidity());
  SSD1306.setCursor(46, 75);
  SSD1306.print("%");
  SSD1306.setCursor(0, 95);
  SSD1306.print("Pressure: ");
  SSD1306.setCursor(8, 105);
  SSD1306.print(bme.readPressure() / 100.0F);
  SSD1306.setCursor(46, 105);
  SSD1306.print("hPa");
	SSD1306.display();
	delay(3000);
}
