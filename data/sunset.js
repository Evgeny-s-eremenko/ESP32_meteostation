const socket = new WebSocket("ws://" + location.hostname + "/ws1");

let nowTime = 0;         // Текущее время (UNIX timestamp)
let sunriseTime = 0;     // Время восхода (секунды с полуночи)
let sunsetTime = 0;      // Время заката (секунды с полуночи)
let sunElevation = 0;    // Высота солнца над горизонтом (градусы)
let solarNoon = 0;       // Время истинного полдня (секунды с полуночи)
let dataInitialized = false;

socket.onmessage = function (event) {
    try {
        const data = JSON.parse(event.data);
        if ("nowTime" in data) nowTime = data.nowTime;
        if ("sunriseTime" in data) sunriseTime = data.sunriseTime;
        if ("sunsetTime" in data) sunsetTime = data.sunsetTime;
        if ("sunElevation" in data) sunElevation = data.sunElevation;
        if ("solarNoon" in data) solarNoon = data.solarNoon;

        dataInitialized = true;
        updateSunLabels();
        updateSunPosition();
    } catch (e) {
        console.error("Ошибка парсинга JSON:", e);
    }
};

// Формат (HH:MM)
function formatTime(secPastMidnight) {
    const h = Math.floor(secPastMidnight / 3600).toString().padStart(2, '0');
    const m = Math.floor((secPastMidnight % 3600) / 60).toString().padStart(2, '0');
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

// Заполняем метки восхода, заката и полдня
function updateSunLabels() {
    document.getElementById('sunriseLabel').textContent = "Восход: " + formatTime(sunriseTime);
    document.getElementById('sunsetLabel').textContent = "Закат: " + formatTime(sunsetTime);
    document.getElementById('solarNoonLabel').textContent = "Полдень: " + formatTime(solarNoon);
}

// Обновляем текущее время и дату
function updateCurrentDateTime() {
    document.getElementById('currentDateTime').textContent = formatDateTime(nowTime);
}

// Движение солнца по дуге + отображение высоты
function updateSunPosition() {
    if (!dataInitialized) return;
    const wrapper = document.getElementById('sunArcWrapper');
    const sunElem = document.getElementById('sun');
    const elevElem = document.getElementById('sunElevation');

    const width = wrapper.clientWidth;
    const height = wrapper.clientHeight;

    // Центр полукруга внизу
    const r = width / 2;
    const cx = r;
    const cy = height;

    // Секунды с полуночи для nowTime
    const date = new Date(nowTime * 1000);
    const secondsPastMidnight = date.getHours() * 3600 + date.getMinutes() * 60 + date.getSeconds();

    if (secondsPastMidnight < sunriseTime || secondsPastMidnight > sunsetTime) {
        // Ночь — солнце под горизонтом
        sunElem.style.left = (width / 2) + 'px';
        sunElem.style.top = height + 'px';
        elevElem.textContent = '';
    } else {
        // Доля дня [0..1]
        const dayProgress = (secondsPastMidnight - sunriseTime) / (sunsetTime - sunriseTime);
        const x = width * dayProgress;
        const dx = x - cx;
        const dy = Math.sqrt(r * r - dx * dx);

        sunElem.style.left = x + 'px';
        sunElem.style.top = (cy - dy) + 'px';

        // Отображаем реальную высоту солнца (с сервера)
        elevElem.textContent = Math.round(sunElevation) + '°';
    }

    nowTime++;
    updateCurrentDateTime();
}

window.addEventListener('load', () => {
    socket.onopen = () => {
        // Отправляем запрос сразу при подключении — без задержки
        if (socket.readyState === WebSocket.OPEN) {
            socket.send("getTime");
        }
    };
    setInterval(() => {
        if (socket.readyState === WebSocket.OPEN) {
            socket.send("getTime");
        }
        updateSunPosition();
    }, 5000);
});
