let chart;
let fetchingData = false; // Флаг для отслеживания выполнения fetch
let dataCounter = 0; // Счетчик для отладки

function getLastNonZeroValue(arr) {
    if (!arr || arr.length === 0) {
        return null;
    }

    for (let i = arr.length - 1; i >= 0; i--) {
        if (arr[i] !== 0) {
            return arr[i];
        }
    }

    return 0;
}

function updateData(data) {
    if (!data || !data.time || !data.temperature || !data.humidity || !data.dewPoint || !data.pressure) {
        console.warn("Некорректные данные для обновления таблицы");
        document.getElementById("temperature").textContent = "Данные отсутствуют";
        document.getElementById("humidity").textContent = "Данные отсутствуют";
        document.getElementById("dewPoint").textContent = "Данные отсутствуют";
        document.getElementById("pressure").textContent = "Данные отсутствуют";
        return;
    }

    const lastTemperature = getLastNonZeroValue(data.temperature);
    const lastHumidity = getLastNonZeroValue(data.humidity);
    const lastDewPoint = getLastNonZeroValue(data.dewPoint);
    const lastPressure = getLastNonZeroValue(data.pressure);
    const lastTime = data.time[data.time.length - 1];

    document.getElementById("temperature").textContent = lastTemperature !== null ? lastTemperature.toFixed(2) + " °C" : "0.00 °C";
    document.getElementById("humidity").textContent = lastHumidity !== null ? lastHumidity.toFixed(2) + " %" : "0.00 %";
    document.getElementById("dewPoint").textContent = lastDewPoint !== null ? lastDewPoint.toFixed(2) + " °C" : "0.00 °C";
    document.getElementById("pressure").textContent = lastPressure !== null ? lastPressure.toFixed(2) + " hPa" : "0.00 hPa";

    if (chart) {
        const MAX_DATA_POINTS = 50;

        const absoluteTime = new Date(lastTime * 1000); // Преобразуем UNIX-время в дату

        chart.data.labels.push(absoluteTime);
        chart.data.datasets[0].data.push(lastTemperature);
        chart.data.datasets[1].data.push(lastHumidity);
        chart.data.datasets[2].data.push(lastDewPoint);
        chart.data.datasets[3].data.push(lastPressure);

        // Удаляем старые данные, если превышено количество точек
        if (chart.data.labels.length > MAX_DATA_POINTS) {
            chart.data.labels.shift();
            chart.data.datasets.forEach(dataset => dataset.data.shift());
        }

        chart.update('none');
    }
}

function createChart(data) {
    const ctx = document.getElementById('chart').getContext('2d');
    if (chart) {
        chart.destroy();
        chart = null; // Убедитесь, что ссылка сбрасывается
    }
    chart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: [],
            datasets: [
                { label: 'Температура (°C)', data: [], borderColor: 'red', fill: false },
                { label: 'Влажность (%)', data: [], borderColor: 'blue', fill: false },
                { label: 'Точка росы (°C)', data: [], borderColor: 'green', fill: false },
                { label: 'Давление (hPa)', data: [], borderColor: 'purple', fill: false },
            ],
        },
        options: {
            animation: false,
            scales: {
                x: {
                    type: 'time',
                    time: {
                        unit: 'second',
                        displayFormats: {
                            second: 'HH:mm:ss',
                        },
                    },
                },
            },
        },
    });
}

function fetchDataAndUpdate() {
    if (fetchingData) return;
    fetchingData = true;

    fetch('/graph-data')
        .then(response => {
            if (!response.ok) {
                throw new Error(`Ошибка сервера: ${response.status}`);
            }
            return response.json();
        })
        .then(data => {
            if (data && data.time && data.time.length > 0) {
                if (!chart) {
                    createChart(data);
                } else { // Обновление графика только если он уже создан
                    updateData(data);
                }
            } else {
                console.warn("Данные о времени отсутствуют, график не может быть создан/обновлен.");
            }
        })
        .catch(error => console.error("Ошибка загрузки данных:", error))
        .finally(() => {
            fetchingData = false;
            setTimeout(fetchDataAndUpdate, 5000);
        });
}

document.addEventListener('DOMContentLoaded', () => {
    const toggleButton = document.getElementById('toggleGraphButton'); // Используем ID
    const chartCanvas = document.getElementById('chart');

    toggleButton.addEventListener('click', () => {
        if (chartCanvas.style.display === 'none') {
            chartCanvas.style.display = 'block';
            if(chart) chart.resize();
        } else {
            chartCanvas.style.display = 'none';
        }
    });

    fetchDataAndUpdate();
    setInterval(fetchDataAndUpdate, 5000);
});
