/*
    Библиотека для определения прогноза погоды по давлению для Arduino
    Документация:
    GitHub: https://github.com/GyverLibs/Forecaster
    Возможности:
    - Определение краткосрочного прогноза погоды по алгоритму Замбретти
    - Принимает давление, температуру, высоту над ур. моря и месяц года
    - Определение тренда давления при помощи линеаризации
    
    AlexGyver, alex@alexgyver.ru
    https://alexgyver.ru/
    MIT License
    
    Основано на
    https://integritext.net/DrKFS/zambretti.htm

    Версии:
    v1.0 - релиз
    v1.1 - добавил вывод тренда давления за 3 часа
    v1.2 - совместимость esp8266/32
*/

#ifndef _Forecaster_h
#define _Forecaster_h
#include <Arduino.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <ArduinoJson.h>

#define _FC_SIZE 6  // Размер буфера. Усреднение за 3 часа, при размере 6 — каждые 30 минут

class Forecaster {
public:
    void begin() {
        if (nvs_flash_init() != ESP_OK) {
            Serial.println("Ошибка инициализации NVS");
        }
        if (nvs_open("storage", NVS_READWRITE, &handle) != ESP_OK) {
            Serial.println("Ошибка открытия NVS");
        } else {
            loadData();
        }
    }

    void setH(int h) {
        H = h * 0.0065f;
    }

    void addP(long P, float t) {
        P = (float)P * pow(1 - H / (t + H + 273.15), -5.257);  // Пересчёт давления на уровень моря
        if (!start) {
            start = true;
            for (uint8_t i = 0; i < _FC_SIZE; i++) Parr[i] = P;
        } else {
            for (uint8_t i = 0; i < (_FC_SIZE - 1); i++) Parr[i] = Parr[i + 1];
            Parr[_FC_SIZE - 1] = P;
        }

        calculateTrend();
        calculateForecast(P / 100);  // Прогноз по давлению в ГПа
        saveData();
    }

    void setMonth(uint8_t month) {
        if (month == 0) season = 0;
        else season = (month >= 4 && month <= 9) ? 2 : 1;
        
        if (month == 12) month = 0;
        month /= 3;                         // 0 зима, 1 весна, 2 лето, 3 осень
        season = month * 0.5 + 1;           // 1, 1.5, 2, 2.5
        if (season == 2.5) season = 1.5;    // 1, 1.5, 2, 1.5
        
    }


    float getCast() {
        return cast;
    }

    int getTrend() {
        return delta;
    }

    void saveData() {
        StaticJsonDocument<256> doc;
        for (int i = 0; i < _FC_SIZE; i++) {
            doc["Parr"][i] = Parr[i];
        }
        doc["cast"] = cast;
        doc["delta"] = delta;

        String jsonString;
        serializeJson(doc, jsonString);

        if (nvs_set_str(handle, "Parr", jsonString.c_str()) == ESP_OK) {
            nvs_commit(handle);
            Serial.println("Данные сохранены в NVS.");
        } else {
            Serial.println("Ошибка сохранения данных.");
        }
    }

private:
    long Parr[_FC_SIZE];
    float H = 0;
    bool start = false;
    float cast = 0;
    int delta = 0;
    uint8_t season = 0;
    nvs_handle handle;

    void calculateTrend() {
        long sumX = 0, sumY = 0, sumX2 = 0, sumXY = 0;
        for (int i = 0; i < _FC_SIZE; i++) {
            sumX += i;
            sumY += Parr[i];
            sumX2 += i * i;
            sumXY += Parr[i] * i;
        }
        float a = _FC_SIZE * sumXY - sumX * sumY;
        a /= _FC_SIZE * sumX2 - sumX * sumX;
        delta = a * (_FC_SIZE - 1);
    }

    void calculateForecast(float P) {
        if (delta > 150) cast = 160 - 0.155 * P - season;
        else if (delta < -150) cast = 130 - 0.124 * P + season;
        else cast = 138 - 0.133 * P;
        if (cast < 0) cast = 0;
    }

    void loadData() {
        size_t required_size;
        if (nvs_get_str(handle, "Parr", NULL, &required_size) == ESP_OK) {
            char* jsonString = new char[required_size];
            if (nvs_get_str(handle, "Parr", jsonString, &required_size) == ESP_OK) {
                StaticJsonDocument<256> doc;
                deserializeJson(doc, jsonString);

                for (int i = 0; i < _FC_SIZE; i++) {
                    Parr[i] = doc["Parr"][i];
                }
                cast = doc["cast"];
                delta = doc["delta"];

                start = true;
                Serial.println("Данные загружены из NVS.");
            }
            delete[] jsonString;
        } else {
            Serial.println("Нет сохранённых данных.");
        }
    }
};

#endif
