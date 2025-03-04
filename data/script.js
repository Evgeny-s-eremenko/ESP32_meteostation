function getColor(value, min, max) {
    const t = (value - min) / (max - min);
    // d3.interpolateRdYlBu выдаёт цвет при t=0 красный, t=1 синий,
    // поэтому, чтобы получить синий при низком значении и красный при высоком, переворачиваем t:
    return d3.interpolateRdYlBu(1 - t);
}

function getColorAirQuality(value, min, max) {
    const t = (value - min) / (max - min);
    // d3.interpolateRdYlBu выдаёт цвет при t=0 красный, t=1 синий,
    // поэтому, чтобы получить синий при низком значении и красный при высоком, переворачиваем t:
    return d3.interpolateRdYlGn(1 - t);
}

function updateIndicator(value, min, max, elementId) {
    const progressBar = document.getElementById(elementId);
    if (!progressBar) {
      console.warn(`Элемент с ID ${elementId} не найден.`);
      return;
    }
    const percentage = ((value - min) / (max - min)) * 100;
    progressBar.style.width = `${percentage}%`;
    progressBar.style.backgroundColor = getColor(value, min, max);
}

function updateIndicatorAirQuality(value, min, max, elementId) {
    const progressBar = document.getElementById(elementId);
    if (!progressBar) {
      console.warn(`Элемент с ID ${elementId} не найден.`);
      return;
    }
    const percentage = ((value - min) / (max - min)) * 100;
    progressBar.style.width = `${percentage}%`;
    progressBar.style.backgroundColor = getColorAirQuality(value, min, max);
}

function updateWeatherIcon(forecast) {
    const weatherIcon = document.getElementById('weatherIcon');
    if (!weatherIcon) {
        console.warn("Элемент weatherIcon не найден.");
        return;
    }

    if (forecast >= 0 && forecast <= 2) {
        weatherIcon.src = '/sunny.png';
    } else if (forecast <= 4.5) {
        weatherIcon.src = '/partly_cloudy.png';
    } else if (forecast <= 7) {
        weatherIcon.src = '/cloudy.png';
    } else {
        weatherIcon.src = '/stormy.png';
    }
}

function updateTrendIcon(trend) {
    const trendIcon = document.getElementById('trendIcon');
    if (!trendIcon) {
        console.warn("Элемент trendIcon не найден.");
        return;
    }

    if (trend < -2.8) {
        trendIcon.src = '/3arrowdown.png';
    } else if (trend >= -2.8 && trend < -1.8) {
        trendIcon.src = '/2arrowdown.png';
    } else if (trend >= -1.8 && trend < -0.7) {
        trendIcon.src = '/arrowdown.png';
    } else if (trend >= -0.7 && trend <= 0.7) {
        trendIcon.src = '/neutral.png';
    } else if (trend > 0.7 && trend <= 1.8) {
        trendIcon.src = '/arrowup.png';
    } else if (trend > 1.8 && trend <= 2.8) {
        trendIcon.src = '/2arrowup.png';
    } else {
        trendIcon.src = '/3arrowup.png';
    }
}

function updateTable(data) {
    if (!data || data.temperature === undefined || data.humidity === undefined || 
        data.dewPoint === undefined || data.pressure === undefined || data.homeTemp === undefined || data.homeHum === undefined || data.homeDP === undefined || data.CO2 === undefined) {
      console.warn("Некорректные данные для обновления таблицы");
      document.getElementById("temperature").textContent = "Данные отсутствуют";
      document.getElementById("humidity").textContent = "Данные отсутствуют";
      document.getElementById("dewPoint").textContent = "Данные отсутствуют";
      document.getElementById("pressure").textContent = "Данные отсутствуют";
      document.getElementById("homeTemp").textContent = "Данные отсутствуют";
      document.getElementById("homeHum").textContent = "Данные отсутствуют";
      document.getElementById("homeDP").textContent = "Данные отсутствуют";
      document.getElementById("CO2").textContent = "Данные отсутствуют";
      return;
    }
  
    document.getElementById("temperature").textContent = data.temperature.toFixed(2) + " °C";
    document.getElementById("humidity").textContent = data.humidity.toFixed(2) + " %";
    document.getElementById("dewPoint").textContent = data.dewPoint.toFixed(2) + " °C";
    document.getElementById("pressure").textContent = data.pressure.toFixed(2) + " hPa";
    document.getElementById("homeTemp").textContent = data.homeTemp.toFixed(2) + " °C";
    document.getElementById("homeHum").textContent = data.homeHum.toFixed(2) + " %";
    document.getElementById("homeDP").textContent = data.homeDP.toFixed(2) + " °C";
    document.getElementById("CO2").textContent = data.CO2.toFixed(2) + " ppm";

    // Обновляем индикаторы
    updateIndicator(data.temperature, -35, 35, 'temperatureBar'); // Температура на улице
    updateIndicator(data.homeTemp, 10, 35, 'homeTempBar'); // Температура дома
    updateIndicator(data.humidity, 0, 100, 'humidityBar'); // Влажность на улице
    updateIndicator(data.homeHum, 0, 100, 'homeHumBar'); // Влажность дома
    updateIndicator(data.dewPoint, -35, 30, 'dewPointBar'); // Точка росы на улице
    updateIndicator(data.homeDP, -20, 30, 'homeDPBar'); // Точка росы дома
    updateIndicator(data.pressure, 956, 1056, 'pressureBar'); // Давление
    updateIndicatorAirQuality(data.CO2, 400, 5000, 'CO2Bar');

    // Обновляем иконку погоды
    updateWeatherIcon(data.forecast);
    updateTrendIcon(data.trend);
  }

function fetchDataAndUpdate() {
    fetch('/graph-data')
        .then(response => {
            if (!response.ok) {
                throw new Error(`Ошибка сервера: ${response.status}`);
            }
            return response.json();
        })
        .then(data => {
            if (data && data.temperature && data.humidity && data.dewPoint && data.pressure && data.homeTemp && data.homeHum && data.homeDP && data.CO2) {
                updateTable(data); // Обновляем только таблицу
            } else {
                console.warn("Данные отсутствуют или некорректны.");
            }
        })
        .catch(error => console.error("Ошибка загрузки данных:", error));
}

document.addEventListener('DOMContentLoaded', () => {
    const toggleButton = document.getElementById('toggleGraphButton');
    const chartContainer = document.getElementById('chartContainer');

    toggleButton.addEventListener('click', () => {
        if (chartContainer.style.display === 'none') {
            chartContainer.style.display = 'block';
        } else {
            chartContainer.style.display = 'none';
        }
    });
    fetchDataAndUpdate(); // Первоначальная загрузка данных для таблицы
    setInterval(fetchDataAndUpdate, 5000); // Периодическое обновление данных для таблицы
});