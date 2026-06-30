// ─────────────────────────────────────────────────────────────
//  script.js — главная страница:
//  цветовые шкалы, индикаторные полосы, иконки прогноза/тренда,
//  периодическое обновление данных с /graph-data.
// ─────────────────────────────────────────────────────────────

// ── URL iframe Grafana (подставьте актуальный адрес сервера) ──

window.addEventListener("DOMContentLoaded", () => {
    const iframe = document.getElementById("temperatureChart");
    const host   = window.location.hostname;

    // Для локальной сети и внешнего доступа используются разные адреса
    iframe.src = host.startsWith("192.168.1.")
        ? "http://192.168.1.214:3000/public-dashboards/8540a49c692f4ba7841a2228b102d203"
        : "http://62.33.134.164:3000/public-dashboards/8540a49c692f4ba7841a2228b102d203";
});

// ── Цветовые шкалы ───────────────────────────────────────────
//
//  Используется локальная реализация палитр ColorBrewer (d3-local-maps.js)
//  вместо подключения полной библиотеки D3.

// Температура и давление: синий (холодно/низкое) → красный (тепло/высокое)
function getColor(value, min, max) {
    const t = (value - min) / (max - min);
    return localInterpolateRdYlBu(1 - t);
}

// Качество воздуха: зелёный (хорошее) → красный (плохое)
function getColorAirQuality(value, min, max) {
    const t = (value - min) / (max - min);
    return localInterpolateRdYlGn(1 - t);
}

// Влажность: диапазон сужен до комфортной зоны [0.3 .. 0.6]
function getColorHumidity(value, min, max) {
    let t = (value - min) / (max - min);
    t = 0.3 + t * 0.3;
    return localInterpolateRdYlBu(t);
}

// ── Обновление индикаторной полосы ───────────────────────────

function updateIndicator(value, min, max, elementId) {
    const bar = document.getElementById(elementId);
    if (!bar) { console.warn("Элемент не найден:", elementId); return; }
    bar.style.width           = ((value - min) / (max - min) * 100) + "%";
    bar.style.backgroundColor = getColor(value, min, max);
}

function updateIndicatorAirQuality(value, min, max, elementId) {
    const bar = document.getElementById(elementId);
    if (!bar) { console.warn("Элемент не найден:", elementId); return; }
    bar.style.width           = ((value - min) / (max - min) * 100) + "%";
    bar.style.backgroundColor = getColorAirQuality(value, min, max);
}

function updateIndicatorHumidity(value, min, max, elementId) {
    const bar = document.getElementById(elementId);
    if (!bar) { console.warn("Элемент не найден:", elementId); return; }
    bar.style.width           = ((value - min) / (max - min) * 100) + "%";
    bar.style.backgroundColor = getColorHumidity(value, min, max);
}

// ── Иконка прогноза погоды (алгоритм Замбретти, 0–10) ────────

function updateWeatherIcon(forecast) {
    const icon = document.getElementById("weatherIcon");
    if (!icon) return;
    if      (forecast <= 2)   icon.src = "/sunny.png";
    else if (forecast <= 4.5) icon.src = "/partly_cloudy.png";
    else if (forecast <= 7)   icon.src = "/cloudy.png";
    else                      icon.src = "/stormy.png";
}

// ── Иконка барического тренда (гПа/3ч) ───────────────────────

function updateTrendIcon(trend) {
    const icon = document.getElementById("trendIcon");
    if (!icon) return;
    if      (trend < -2.8)              icon.src = "/3arrowdown.png";
    else if (trend >= -2.8 && trend < -1.8) icon.src = "/2arrowdown.png";
    else if (trend >= -1.8 && trend < -0.7) icon.src = "/arrowdown.png";
    else if (trend >= -0.7 && trend <= 0.7) icon.src = "/neutral.png";
    else if (trend >  0.7  && trend <= 1.8) icon.src = "/arrowup.png";
    else if (trend >  1.8  && trend <= 2.8) icon.src = "/2arrowup.png";
    else                                icon.src = "/3arrowup.png";
}

// ── Обновление всех виджетов данными с сервера ───────────────

function updateTable(data) {
    // Уличные данные
    document.getElementById("temperature").textContent = data.temperature.toFixed(2) + " °C";
    document.getElementById("humidity")   .textContent = data.humidity   .toFixed(2) + " %";
    document.getElementById("dewPoint")   .textContent = data.dewPoint   .toFixed(2) + " °C";

    // Кайма Температура-Улица по FAN
    const outdoorTempWidget = document.getElementById("outdoorTempWidget");
    if (outdoorTempWidget) {
        outdoorTempWidget.style.setProperty(
            "--left-border-color",
            data.FAN === 1 ? "#00cc00" : "transparent"
        );
    }

    // Кайма Влажность-Улица по HEAT
    const outdoorHumWidget = document.getElementById("outdoorHumWidget");
    if (outdoorHumWidget) {
        let borderColor = "transparent";

        if (data.HEAT === 2) {
            borderColor = "#ff0000";
        } else if (data.HEAT === 3) {
            borderColor = "#1194ff";
        }

        outdoorHumWidget.style.setProperty("--left-border-color", borderColor);
    }

    // Домашние данные
    document.getElementById("homeTemp").textContent = data.homeTemp.toFixed(2) + " °C";
    document.getElementById("homeHum") .textContent = data.homeHum .toFixed(2) + " %";
    document.getElementById("homeDP")  .textContent = data.homeDP  .toFixed(2) + " °C";

    // Давление
    document.getElementById("pressure").textContent = data.pressure.toFixed(2) + " hPa";

    // Качество воздуха
    document.getElementById("CO2") .textContent = data.CO2 .toFixed(0) + " ppm";
    document.getElementById("TVOC").textContent = data.TVOC.toFixed(0) + " ppb";

    // PM2.5 и PM10
    document.getElementById("PM25").textContent = data["PM2.5"].toFixed(1) + " µg/m³";
    document.getElementById("PM10").textContent = data["PM10"].toFixed(1) + " µg/m³";

    // Свет и УФ
    document.getElementById("LUX").textContent = data.LUX.toFixed(1) + " lux";
    document.getElementById("UV") .textContent = data.UV .toFixed(2);

    // Индикаторные полосы
    updateIndicator(        data.temperature, -35,  35, "temperatureBar");
    updateIndicator(        data.homeTemp,     10,  35, "homeTempBar");
    updateIndicatorHumidity(data.humidity,      0, 100, "humidityBar");
    updateIndicatorHumidity(data.homeHum,       0, 100, "homeHumBar");
    updateIndicator(        data.dewPoint,    -35,  30, "dewPointBar");
    updateIndicator(        data.homeDP,      -20,  30, "homeDPBar");
    updateIndicator(        data.pressure,    956, 1056, "pressureBar");
    updateIndicatorAirQuality(data.CO2,  400, 2000, "CO2Bar");
    updateIndicatorAirQuality(data.TVOC,   0, 2000, "TVOCBar");
    updateIndicatorAirQuality(data["PM2.5"], 0, 20, "PM25Bar");
    updateIndicatorAirQuality(data["PM10"],  0, 20, "PM10Bar");

    // Иконки прогноза и тренда
    updateWeatherIcon(data.forecast);
    updateTrendIcon(data.trend);
}

// ── Опрос /graph-data каждые 5 секунд ────────────────────────

function fetchDataAndUpdate() {
    fetch("/graph-data", { cache: "no-store" })
        .then(r => r.json())
        .then(data => updateTable(data))
        .catch(err => console.error("Ошибка загрузки данных:", err));
}

document.addEventListener("DOMContentLoaded", () => {
    // Кнопка показа/скрытия графика Grafana
    const toggleBtn      = document.getElementById("toggleGraphButton");
    const chartContainer = document.getElementById("chartContainer");
    toggleBtn.addEventListener("click", () => {
        chartContainer.style.display =
            (chartContainer.style.display === "none") ? "block" : "none";
    });

    // Первоначальная загрузка и периодическое обновление
    fetchDataAndUpdate();
    setInterval(fetchDataAndUpdate, 5000);
});