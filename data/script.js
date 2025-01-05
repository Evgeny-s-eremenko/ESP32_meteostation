function updateTable(data) {
    if (!data || !data.temperature || !data.humidity || !data.dewPoint || !data.pressure) {
        console.warn("Некорректные данные для обновления таблицы");
        document.getElementById("temperature").textContent = "Данные отсутствуют";
        document.getElementById("humidity").textContent = "Данные отсутствуют";
        document.getElementById("dewPoint").textContent = "Данные отсутствуют";
        document.getElementById("pressure").textContent = "Данные отсутствуют";
        return;
    }

    document.getElementById("temperature").textContent = data.temperature[data.temperature.length - 1].toFixed(2) + " °C"; // Последнее значение
    document.getElementById("humidity").textContent = data.humidity[data.humidity.length - 1].toFixed(2) + " %";
    document.getElementById("dewPoint").textContent = data.dewPoint[data.dewPoint.length - 1].toFixed(2) + " °C";
    document.getElementById("pressure").textContent = data.pressure[data.pressure.length - 1].toFixed(2) + " hPa";
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
            if (data && data.temperature && data.humidity && data.dewPoint && data.pressure) {
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