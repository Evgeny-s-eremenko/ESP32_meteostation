const socket = new WebSocket("ws://" + location.hostname + "/ws1");

let nowTime = 0;
let sunriseTime = 0;
let sunsetTime = 0;
let sunElevation = 0;
let solarNoon = 0;
let dataInitialized = false;
let canvasReady = false;

socket.onmessage = function (event) {
    try {
        const data = JSON.parse(event.data);
        if ("nowTime" in data) nowTime = data.nowTime;
        if ("sunriseTime" in data) sunriseTime = data.sunriseTime;
        if ("sunsetTime" in data) sunsetTime = data.sunsetTime;
        if ("sunElevation" in data) sunElevation = data.sunElevation;
        if ("solarNoon" in data) solarNoon = data.solarNoon;

        dataInitialized = true;
        initCanvas();
        updateSunLabels();
        drawSunArc();
        updateSunPosition();
    } catch (e) {
        console.error("Ошибка парсинга JSON:", e);
    }
};

function formatTime(secPastMidnight) {
    const h = Math.floor(secPastMidnight / 3600).toString().padStart(2, '0');
    const m = Math.floor((secPastMidnight % 3600) / 60).toString().padStart(2, '0');
    return h + ":" + m;
}

function formatDateTime(unixSec) {
    const date = new Date(unixSec * 1000);
    const year = date.getFullYear();
    const month = (date.getMonth() + 1).toString().padStart(2, '0');
    const day = date.getDate().toString().padStart(2, '0');
    const hh = date.getHours().toString().padStart(2, '0');
    const mm = date.getMinutes().toString().padStart(2, '0');
    return year + "-" + month + "-" + day + " " + hh + ":" + mm;
}

function formatDuration(seconds) {
    var h = Math.floor(seconds / 3600);
    var m = Math.floor((seconds % 3600) / 60);
    return h + " ч " + m.toString().padStart(2, '0') + " мин";
}

function updateSunLabels() {
    var el;
    el = document.getElementById('sunriseLabel');
    if (el) el.textContent = "Восход: " + formatTime(sunriseTime);
    el = document.getElementById('sunsetLabel');
    if (el) el.textContent = "Закат: " + formatTime(sunsetTime);
    el = document.getElementById('solarNoonLabel');
    if (el) el.textContent = "Полдень: " + formatTime(solarNoon);

    var dayLen = sunsetTime - sunriseTime;
    var nightLen = 86400 - dayLen;
    el = document.getElementById('dayLength');
    if (el) el.textContent = formatDuration(dayLen);
    el = document.getElementById('nightLength');
    if (el) el.textContent = formatDuration(nightLen);
}

function updateCurrentDateTime() {
    var el = document.getElementById('currentDateTime');
    if (el) el.textContent = formatDateTime(nowTime);
}

function initCanvas() {
    if (canvasReady) return;
    var canvas = document.getElementById('sunCanvas');
    if (!canvas) return;
    var dpr = window.devicePixelRatio || 1;
    var rect = canvas.getBoundingClientRect();
    canvas.width = rect.width * dpr;
    canvas.height = rect.height * dpr;
    var ctx = canvas.getContext('2d');
    ctx.scale(dpr, dpr);
    canvasReady = true;
}

function drawSunArc() {
    if (!dataInitialized) return;
    var canvas = document.getElementById('sunCanvas');
    if (!canvas) return;
    var ctx = canvas.getContext('2d');
    var dpr = window.devicePixelRatio || 1;
    var W = canvas.width / dpr;
    var H = canvas.height / dpr;

    ctx.clearRect(0, 0, W, H);

    var horizonY = Math.round(H * 0.65);
    var amplitude = Math.round(H * 0.55);
    var cx = W * solarNoon / 86400;

    function curveY(x) {
        var v = Math.cos((x / cx - 1) * Math.PI);
        return horizonY - amplitude * v;
    }

    ctx.beginPath();
    ctx.setLineDash([6, 4]);
    ctx.strokeStyle = 'rgba(255,255,255,0.3)';
    ctx.lineWidth = 1;
    ctx.moveTo(0, horizonY);
    ctx.lineTo(W, horizonY);
    ctx.stroke();
    ctx.setLineDash([]);

    var hours = [0, 6, 12, 18];
    ctx.beginPath();
    ctx.setLineDash([4, 4]);
    ctx.strokeStyle = 'rgba(255,255,255,0.2)';
    ctx.lineWidth = 1;
    for (var i = 0; i < hours.length; i++) {
        var x = Math.round(W * hours[i] / 24);
        ctx.moveTo(x, 0);
        ctx.lineTo(x, horizonY);
    }
    ctx.stroke();
    ctx.setLineDash([]);

    ctx.fillStyle = '#aaaaaa';
    ctx.font = '10px sans-serif';
    ctx.textAlign = 'center';
    for (var i = 0; i < hours.length; i++) {
        var x = Math.round(W * hours[i] / 24);
        var label = hours[i].toString().padStart(2, '0');
        ctx.fillText(label, x, horizonY + 14);
    }

    ctx.fillStyle = 'rgba(80, 70, 50, 0.15)';
    ctx.fillRect(0, horizonY, W, H - horizonY);

    ctx.beginPath();
    ctx.strokeStyle = 'rgba(255,255,255,0.5)';
    ctx.lineWidth = 2;
    var step = W / 288;
    for (var i = 0; i <= 288; i++) {
        var x = i * step;
        var y = curveY(x);
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
    }
    ctx.stroke();
}

function updateSunPosition() {
    if (!dataInitialized) return;
    var canvas = document.getElementById('sunCanvas');
    var sunElem = document.getElementById('sun');
    var elevElem = document.getElementById('sunElevation');
    if (!canvas || !sunElem) return;

    var dpr = window.devicePixelRatio || 1;
    var W = canvas.width / dpr;
    var H = canvas.height / dpr;
    var horizonY = Math.round(H * 0.65);
    var amplitude = Math.round(H * 0.55);
    var cx = W * solarNoon / 86400;

    var date = new Date(nowTime * 1000);
    var sec = date.getHours() * 3600 + date.getMinutes() * 60 + date.getSeconds();

    var sunX = W * sec / 86400;
    var v = Math.cos((sunX / cx - 1) * Math.PI);
    var sunY = horizonY - amplitude * v;

    var canvasRect = canvas.getBoundingClientRect();
    var cssX = canvasRect.left - canvasRect.left + (sunX / W) * canvasRect.width;
    var cssY = canvasRect.top - canvasRect.top + (sunY / H) * canvasRect.height;

    sunElem.style.left = cssX + 'px';
    sunElem.style.top = cssY + 'px';

    if (sec < sunriseTime || sec > sunsetTime) {
        elevElem.textContent = '';
    } else {
        elevElem.textContent = Math.round(sunElevation) + '\u00B0';
    }

    nowTime++;
    updateCurrentDateTime();
}

window.addEventListener('load', function () {
    socket.onopen = function () {
        if (socket.readyState === WebSocket.OPEN) {
            socket.send("getTime");
        }
    };
    setInterval(function () {
        if (socket.readyState === WebSocket.OPEN) {
            socket.send("getTime");
        }
        updateSunPosition();
    }, 10000);
});
