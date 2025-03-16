const socket = new WebSocket("ws://" + location.hostname + "/ws1");

let nowTime = 0;       // Текущее время (UNIX timestamp), по умолчанию 0
let sunriseTime = 0;   // Время восхода (секунды с полуночи)
let sunsetTime = 0;    // Время заката (секунды с полуночи)

socket.onmessage = function (event) {
    try {
        const data = JSON.parse(event.data);
        if ("nowTime" in data) nowTime = data.nowTime;
        if ("sunriseTime" in data) sunriseTime = data.sunriseTime;
        if ("sunsetTime" in data) sunsetTime = data.sunsetTime;

        updateSunLabels();
        updateSunPosition();
    } catch (e) {
        console.error("Ошибка парсинга JSON:", e);
    }
};

// Формат (HH:MM)
function formatTime(secPastMidnight) {
    const h = Math.floor(secPastMidnight / 3600).toString().padStart(2, '0'); // секунды -> часы
    const m = Math.floor((secPastMidnight % 3600) / 60).toString().padStart(2, '0'); // секунды -> минуты
    return `${h}:${m}`;
}

// Формат (YYYY-MM-DD HH:MM)
function formatDateTime(unixSec) {
    const date = new Date(unixSec * 1000);
    const year = date.getFullYear();
    const month = (date.getMonth() + 1).toString().padStart(2, '0');
    const day = date.getDate().toString().padStart(2, '0');
    const hh = date.getHours().toString().padStart(2, '0');
    const mm = date.getMinutes().toString().padStart(2, '0');
    return `${year}-${month}-${day} ${hh}:${mm}`;
}

// Заполняем метки восхода и заката
function updateSunLabels() {
    document.getElementById('sunriseLabel').textContent = "Восход: " + formatTime(sunriseTime);
    document.getElementById('sunsetLabel').textContent = "Закат: " + formatTime(sunsetTime);
}

// Обновляем текущее время и дату
function updateCurrentDateTime() {
    document.getElementById('currentDateTime').textContent = formatDateTime(nowTime);
}

// Движение солнца по дуге
function updateSunPosition() {
    const wrapper = document.getElementById('sunArcWrapper');
    const sunElem = document.getElementById('sun');

    const width = wrapper.clientWidth;   // 300
    const height = wrapper.clientHeight; // 150

    // Центр полукруга внизу
    const r = width / 2;
    const cx = r;
    const cy = height;

    // Вычисляем секунды с полуночи для nowTime
    const date = new Date(nowTime * 1000);
    const secondsPastMidnight = date.getHours() * 3600 + date.getMinutes() * 60 + date.getSeconds();

    if (secondsPastMidnight < sunriseTime || secondsPastMidnight > sunsetTime) {
        // Ночь — солнце под горизонтом
        sunElem.style.left = (width / 2) + 'px';
        sunElem.style.top = height + 'px';
    } else {
        // Доля дня [0..1]
        const dayProgress = (secondsPastMidnight - sunriseTime) / (sunsetTime - sunriseTime);
        const x = width * dayProgress;
        const dx = x - cx;
        const dy = Math.sqrt(r * r - dx * dx);

        sunElem.style.left = x + 'px';
        sunElem.style.top = (cy - dy) + 'px';
    }

    nowTime++; // Локально увеличиваем секунды
    updateCurrentDateTime();
}
window.addEventListener('load', () => {
    socket.onopen = () => {
        socket.send("getTime");
    };
    updateSunPosition();
    setInterval(() => {
        if (socket.readyState === WebSocket.OPEN) {
            socket.send("getTime"); // Запрос времени у ESP
        }
        updateSunPosition();
    }, 60000);
});