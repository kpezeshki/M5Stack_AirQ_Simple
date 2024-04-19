#include <Arduino.h>
#include <Wire.h>

#include <SensirionI2CScd4x.h>
#include <SensirionI2CSen5x.h>
#include <Adafruit_NeoPixel.h>
#include <M5Unified.h>
#include <lgfx/v1/panel/Panel_GDEW0154D67.hpp>

#include "config.h"

#define USE_BUZZER true
#define NUM_SAMPLES_BETWEEN_DISPLAYS 20 // about 5 minutes between refreshes
#define SAMPLE_DELAY 6000 // this is minimum sample interval for SCD41. There's enough other crap in the loop that we have some overhead
#define SERIAL_DEBUG false

// sensor sums
float hum_sum = 0.0;
float tmp_sum = 0.0;
float co2_sum = 0.0;
float voc_sum = 0.0;
float nox_sum = 0.0;

float pm25_sum = 0.0;
float pm10_sum = 0.0;

int num_samples = 0;

// hardware instantiation

SensirionI2CScd4x scd4x;
SensirionI2CSen5x sen5x;
Adafruit_NeoPixel pixels(3, GROVE_SDA, NEO_GRB + NEO_KHZ800);

class AirQ_GFX : public lgfx::LGFX_Device {
    lgfx::Panel_GDEW0154D67 _panel_instance;
    lgfx::Bus_SPI           _spi_bus_instance;

   public:
    AirQ_GFX(void) {
        {
            auto cfg = _spi_bus_instance.config();

            cfg.pin_mosi   = 6;
            cfg.pin_miso   = -1;
            cfg.pin_sclk   = 5;
            cfg.pin_dc     = 3;
            cfg.freq_write = 40000000;

            _spi_bus_instance.config(cfg);
            _panel_instance.setBus(&_spi_bus_instance);
        }
        {
            auto cfg = _panel_instance.config();

            cfg.invert       = false;
            cfg.pin_cs       = 4;
            cfg.pin_rst      = 2;
            cfg.pin_busy     = 1;
            cfg.panel_width  = 200;
            cfg.panel_height = 200;
            cfg.offset_x     = 0;
            cfg.offset_y     = 0;

            _panel_instance.config(cfg);
        }
        setPanel(&_panel_instance);
    }
    bool begin(void) { return init_impl(true , false); }
};

AirQ_GFX lcd;
M5Canvas canvas(&lcd);

int loopIndex = 0;

// per-cycle sensor values

// SCD4x
float temperature = 0.0f;
float humidity = 0.0f;

// SEN5x (again copied from sensirion github)

float massConcentrationPm1p0;
float massConcentrationPm2p5;
float massConcentrationPm4p0;
float massConcentrationPm10p0;
float ambientHumidity;
float ambientTemperature;
float vocIndex;
float noxIndex;

// max values

float co2max = 0.0;
float pm25max = 0.0;
float pm10max  = 0.0;
float vocmax = 0.0;
float noxmax = 0.0;

// safe threshold values

float co2threshold = 1500.0;

float pm25threshold = 10.0;
float pm10threshold  = 40.0;

float vocthreshold = 400.0;
float noxthreshold = 10.0;

bool display_on_next = true;

void setup() {

    pinMode(USER_BTN_B, INPUT_PULLUP);

    if (SERIAL_DEBUG) Serial.begin(115200);

    if (SERIAL_DEBUG) Serial.println("AirQ Simple Demo");

    if (SERIAL_DEBUG) Serial.println("Turning on power...");
    pinMode(POWER_HOLD, OUTPUT);
    digitalWrite(POWER_HOLD, HIGH);
    pinMode(SEN55_POWER_EN, OUTPUT);
    digitalWrite(SEN55_POWER_EN, LOW);

    // buzzer setup
    ledcSetup(0, 1000, 8);

    if (USE_BUZZER) {
        ledcAttachPin(BUZZER_PIN, 0);
        ledcWrite(0, 128);
        delay(200);
        ledcWrite(0,0);
        ledcDetachPin(BUZZER_PIN);
    }

    if (SERIAL_DEBUG) Serial.println("Enabling devices...");
    pinMode(GROVE_SDA, OUTPUT);
    pinMode(GROVE_SCL, OUTPUT);
    Wire.begin(I2C1_SDA_PIN, I2C1_SCL_PIN);
    delay(500);
    pixels.begin();
    for (int i = 0; i < 3; i++) pixels.setPixelColor(i, pixels.Color(255,255,0));
    pixels.setBrightness(10);
    pixels.show();
    lcd.begin();
    lcd.setTextSize(2);

    if (SERIAL_DEBUG) Serial.println(" SEN4x");
    scd4x.begin(Wire);
    delay(500);
    scd4x.stopPeriodicMeasurement();
    delay(500);
    scd4x.startPeriodicMeasurement();

    if (SERIAL_DEBUG) Serial.println("  SEN5x");
    sen5x.begin(Wire);
    delay(500);
    sen5x.deviceReset();
    delay(500);
    sen5x.startMeasurement();

    if (SERIAL_DEBUG) Serial.println("  Waiting 10 seconds for sensors");
    delay(10000);
}

void loop() {

    if (SERIAL_DEBUG) Serial.println("Collecting Sensor Data");

    // SCD4x (copied from the sensirion library example)
    uint16_t error;
    char errorMessage[256];

    if (SERIAL_DEBUG) Serial.println("SCD4x");
    uint16_t co2 = 0;
    bool isDataReady = false;
    error = scd4x.getDataReadyFlag(isDataReady);
    if (error && SERIAL_DEBUG) {
        Serial.print("Error trying to execute getDataReadyFlag(): ");
        errorToString(error, errorMessage, 256);
        Serial.println(errorMessage);
        return;
    }
    if (!isDataReady) {
        return;
    }
    error = scd4x.readMeasurement(co2, temperature, humidity);

    if (SERIAL_DEBUG) {
        if (error) {
            Serial.print("Error trying to execute readMeasurement(): ");
            errorToString(error, errorMessage, 256);
            Serial.println(errorMessage);
        } else if (co2 == 0) {
            Serial.println("Invalid sample detected, skipping.");
        } else {
            Serial.print("Co2:");
            Serial.print(co2);
            Serial.print("\n");
            Serial.print("Temperature:");
            Serial.print(temperature);
            Serial.print("\n");
            Serial.print("Humidity:");
            Serial.println(humidity);
        }
    }

    if (SERIAL_DEBUG) Serial.println("SEN5x");
    error = sen5x.readMeasuredValues(
        massConcentrationPm1p0, massConcentrationPm2p5, massConcentrationPm4p0,
        massConcentrationPm10p0, ambientHumidity, ambientTemperature, vocIndex,
        noxIndex);

    if (SERIAL_DEBUG) {
        if (error) {
            Serial.print("Error trying to execute readMeasuredValues(): ");
            errorToString(error, errorMessage, 256);
            Serial.println(errorMessage);
        } else {
            Serial.print("MassConcentrationPm1p0:");
            Serial.print(massConcentrationPm1p0);
            Serial.print("\n");
            Serial.print("MassConcentrationPm2p5:");
            Serial.print(massConcentrationPm2p5);
            Serial.print("\n");
            Serial.print("MassConcentrationPm4p0:");
            Serial.print(massConcentrationPm4p0);
            Serial.print("\n");
            Serial.print("MassConcentrationPm10p0:");
            Serial.print(massConcentrationPm10p0);
            Serial.print("\n");
            Serial.print("AmbientHumidity:");
            if (isnan(ambientHumidity)) {
                Serial.print("n/a");
            } else {
                Serial.print(ambientHumidity);
            }
            Serial.print("\n");
            Serial.print("AmbientTemperature:");
            if (isnan(ambientTemperature)) {
                Serial.print("n/a");
            } else {
                Serial.print(ambientTemperature);
            }
            Serial.print("\n");
            Serial.print("VocIndex:");
            if (isnan(vocIndex)) {
                Serial.print("n/a");
            } else {
                Serial.print(vocIndex);
            }
            Serial.print("\n");
            Serial.print("NoxIndex:");
            if (isnan(noxIndex)) {
                Serial.println("n/a");
            } else {
                Serial.println(noxIndex);
            }
        }
    }

    // now let's turn a light on when we're above various thresholds

    bool safe_co2 = true;
    bool safe_pm  = true;
    bool safe_gas = true;

    bool co2_ready = false;
    bool pm_ready  = false;
    bool gas_ready = false;

    int brightness = 10;

    // check if sensors are ready
    if (co2 > 0.0) co2_ready = true;
    if (massConcentrationPm2p5 > 0.0 && massConcentrationPm10p0 > 0.0) pm_ready = true;
    if (vocIndex > 0.0 && noxIndex > 0.0) gas_ready = true;

    // if sensors are ready, update averages
    if (co2_ready && pm_ready && gas_ready) {
        hum_sum += ambientHumidity;
        tmp_sum += ambientTemperature;
        co2_sum += co2;
        voc_sum += vocIndex;
        nox_sum += noxIndex;
        pm25_sum += massConcentrationPm2p5;
        pm10_sum += massConcentrationPm10p0;
        num_samples += 1;
    }

    // check if levels are safe
    if (co2 > co2threshold) safe_co2 = false;

    if (massConcentrationPm2p5 > pm25threshold) safe_pm = false;
    if (massConcentrationPm10p0 > pm10threshold) safe_pm = false;

    if (vocIndex > vocthreshold) safe_gas = false;
    if (noxIndex > noxthreshold) safe_gas = false;

    if (!(safe_co2 && safe_pm && safe_gas)) brightness = 255;

    // update max levels

    if (co2 > co2max) co2max = co2;
    if (massConcentrationPm2p5 > pm25max) pm25max = massConcentrationPm2p5;
    if (massConcentrationPm10p0 > pm10max) pm10max = massConcentrationPm10p0;
    if (vocIndex > vocmax) vocmax = vocIndex;
    if (noxIndex > noxmax) noxmax = noxIndex;

    // update led stack

    pixels.clear();
    for (int i = 0; i < 3; i++) pixels.setPixelColor(i, pixels.Color(255,255,0));
    pixels.show();

    delay(200);

    pixels.clear();

    if (!co2_ready) {
        pixels.setPixelColor(0, pixels.Color(255,255,0));
        if (SERIAL_DEBUG) Serial.println("CO2 not ready...");
    }
    else if (safe_co2) pixels.setPixelColor(0, pixels.Color(0,255,0));
    else pixels.setPixelColor(0, pixels.Color(255,0,0));

    if (!pm_ready) {
        pixels.setPixelColor(1, pixels.Color(255,255,0));
        if (SERIAL_DEBUG) Serial.println("Particulate not ready...");
    }
    else if (safe_pm) pixels.setPixelColor(1, pixels.Color(0,128,128));
    else pixels.setPixelColor(1, pixels.Color(255,0,0));

    if (!gas_ready) {
        pixels.setPixelColor(2, pixels.Color(255,255,0));
        if (SERIAL_DEBUG) Serial.println("VOC / NOx not ready...");
    }
    else if (safe_gas) pixels.setPixelColor(2, pixels.Color(0,0,255));
    else pixels.setPixelColor(2, pixels.Color(255,0,0));

    pixels.setBrightness(brightness);
    pixels.show();

    // buzz if out of range

    if (!(safe_co2 && safe_pm && safe_gas)) {
        if (USE_BUZZER) {
            ledcAttachPin(BUZZER_PIN, 0);
            ledcWrite(0, 128);
            delay(1000);
            ledcWrite(0,0);
            ledcDetachPin(BUZZER_PIN);
            display_on_next = true;
        }
    }

    // Display sensor values on the screen if button B is not pressed
    if (SERIAL_DEBUG) {
        Serial.print("Display sensor values? ");
        Serial.println(display_on_next);

        Serial.print("Num samples? ");
        Serial.println(num_samples);
    }


    // display secondary screen with battery voltages and maxes 
    if (digitalRead(USER_BTN_B) == 0) {
        if (SERIAL_DEBUG) Serial.println("UPDATING DISPLAY WITH STATUS");

        canvas.createSprite(lcd.width(), lcd.height());
        canvas.clear(TFT_WHITE);
        canvas.setTextColor(TFT_BLACK, TFT_WHITE);
        canvas.setCursor(0, 0);
        canvas.setTextSize(2);

        int battery_level = 2*analogRead(14);
        float battery_level_voltage = 3.3*((float) battery_level) / 4095.0;

        canvas.drawString("BAT: " + String(battery_level_voltage) + " V", 0, 0);

        canvas.drawString("---MAX LEVELS---", 0, 30);
        canvas.drawString("CO2:" + String(co2max) + " ppm", 0, 60);
        canvas.drawString("VOC:" + String(vocmax), 0, 90);
        canvas.drawString("NOx:" + String(noxmax), 0, 120);        
        canvas.drawString("PM2.5:" + String(pm25max, 1) + " ug/m3", 0, 150);
        canvas.drawString("PM10:" + String(pm10max, 1) + " ug/m3", 0, 180);

        canvas.pushSprite(&lcd, 0, 0);
        lcd.display();

        display_on_next = true;
        loopIndex = 0;

        delay(5000);

    }

    else if (display_on_next) {
        if (SERIAL_DEBUG) Serial.println("UPDATING DISPLAY WITH NEW SENSOR VALUES");

        canvas.createSprite(lcd.width(), lcd.height());
        canvas.clear(TFT_WHITE);
        canvas.setTextColor(TFT_BLACK, TFT_WHITE);
        canvas.setCursor(0, 0);
        canvas.setTextSize(2);

        if (num_samples > 1) {
            canvas.drawString("AVG", 160, 0);
            canvas.drawString(String(num_samples), 170, 25);
        }

        float hum_avg = ambientHumidity;
        float tmp_avg = ambientTemperature;
        float co2_avg = co2;
        float voc_avg = vocIndex;
        float nox_avg = noxIndex;
        float pm25_avg = massConcentrationPm2p5;
        float pm10_avg = massConcentrationPm10p0;

        if (num_samples > 1) {
            hum_avg = hum_sum / (float) num_samples;
            tmp_avg = tmp_sum / (float) num_samples;
            co2_avg = co2_sum / (float) num_samples;
            voc_avg = voc_sum / (float) num_samples;
            nox_avg = nox_sum / (float) num_samples;
            pm25_avg = pm25_sum / (float) num_samples;
            pm10_avg = pm10_sum / (float) num_samples;

            // set all sums to 0
            hum_sum = 0;
            tmp_sum = 0;
            co2_sum = 0;
            voc_sum = 0;
            nox_sum = 0;
            pm25_sum = 0;
            pm10_sum = 0;

            num_samples = 0;
        }

        canvas.drawString("HUM:" + String(hum_avg) + " %", 0, 0);
        canvas.drawString("TMP:" + String(tmp_avg) + " C", 0, 30);

        if (safe_co2) canvas.drawString("CO2:" + String(co2_avg) + " ppm", 0, 60);
        else canvas.drawString(">CO2:" + String(co2) + " ppm", 0, 60);
        if (safe_gas) {
            canvas.drawString("VOC:" + String(voc_avg), 0, 90);
            canvas.drawString("NOx:" + String(nox_avg), 0, 120);
        }
        else {
            canvas.drawString(">VOC:" + String(vocIndex), 0, 90);
            canvas.drawString(">NOx:" + String(noxIndex), 0, 120);         
        }
        if (safe_pm) {
            canvas.drawString("PM2.5:" + String(pm25_avg, 1) + " ug/m3", 0, 150);
            canvas.drawString("PM10:" + String(pm10_avg, 1) + " ug/m3", 0, 180);
        }
        else {
            canvas.drawString(">PM2.5:" + String(massConcentrationPm2p5, 1) + " ug/m3", 0, 150);
            canvas.drawString(">PM10:" + String(massConcentrationPm10p0, 1) + " ug/m3", 0, 180);            
        }

        canvas.pushSprite(&lcd, 0, 0);
        lcd.display();

        if (!(safe_co2 && safe_pm && safe_gas)) display_on_next = true;
        else display_on_next = false;
        
    }

    if (SERIAL_DEBUG) Serial.println("WAITING");
    delay(SAMPLE_DELAY);

    if (loopIndex % (NUM_SAMPLES_BETWEEN_DISPLAYS) == 0) display_on_next = true;

    loopIndex += 1;
}