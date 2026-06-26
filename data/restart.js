// ─────────────────────────────────────────────────────────────
//  restart.js — логика страницы администратора:
//  WebSocket статус задач, таск-менеджер, системная информация,
//  управление nRF905, команды на внешний блок STM32.
// ─────────────────────────────────────────────────────────────

// ── Кнопка перезагрузки ESP32 ────────────────────────────────

document.getElementById("RestartButton").addEventListener("click", function (e) {
    e.preventDefault();
    fetch("/restart", { method: "POST" })
        .then(r => r.text())
        .then(text => { document.getElementById("result").innerText = text; })
        .catch(err => console.error("Ошибка перезагрузки:", err));
});

// ── WebSocket /ws — обновление состояния задач в реальном времени

const socket = new WebSocket("ws://" + location.hostname + "/ws");

socket.onmessage = function (event) {
    try {
        const data = JSON.parse(event.data);
        updateButtonState("TaskNRF905",     data.nRF905);
        updateButtonState("TaskCO2",        data.CO2);
        updateButtonState("TaskNextion",    data.nextion);
        updateButtonState("TaskBMP280",     data.BMP280);
        updateButtonState("TaskInfluxDB",   data.InfluxDB);
        updateButtonState("TaskForecaster", data.Forecaster);
        updateButtonState("TaskNTP",        data.NTP);
        updateButtonState("TaskTVOC",       data.TVOC);
    } catch (err) {
        console.error("Ошибка парсинга JSON задач:", err);
    }
};

// ── Разовый HTTP-запрос состояния задач при загрузке страницы

async function fetchTaskStates() {
    try {
        const response = await fetch("/getTasksState");
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const data = await response.json();
        updateButtonState("TaskNRF905",     data.nRF905);
        updateButtonState("TaskCO2",        data.CO2);
        updateButtonState("TaskNextion",    data.nextion);
        updateButtonState("TaskBMP280",     data.BMP280);
        updateButtonState("TaskInfluxDB",   data.InfluxDB);
        updateButtonState("TaskForecaster", data.Forecaster);
        updateButtonState("TaskNTP",        data.NTP);
        updateButtonState("TaskTVOC",       data.TVOC);
    } catch (err) {
        console.error("Ошибка получения состояния задач:", err);
    }
}

// ── Обновление визуального состояния кнопки-тумблера

function updateButtonState(taskId, isRunning) {
    const btn = document.getElementById("btn" + taskId);
    if (!btn) return;
    if (isRunning) {
        btn.classList.replace("stopped", "running");
        btn.textContent = btn.textContent.replace("Stopped", "Running");
    } else {
        btn.classList.replace("running", "stopped");
        btn.textContent = btn.textContent.replace("Running", "Stopped");
    }
}

// ── Отправка команды переключения задачи на сервер

function toggleTask(taskName) {
    fetch("/toggleTask", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: "task=" + taskName
    }).catch(err => console.error("Ошибка переключения задачи:", err));
}

// ── Привязка кнопок таск-менеджера к функции переключения

document.getElementById("btnTaskNRF905")    .addEventListener("click", () => toggleTask("nRF905"));
document.getElementById("btnTaskCO2")       .addEventListener("click", () => toggleTask("CO2"));
document.getElementById("btnTaskNextion")   .addEventListener("click", () => toggleTask("nextion"));
document.getElementById("btnTaskBMP280")    .addEventListener("click", () => toggleTask("BMP280"));
document.getElementById("btnTaskInfluxDB")  .addEventListener("click", () => toggleTask("InfluxDB"));
document.getElementById("btnTaskForecaster").addEventListener("click", () => toggleTask("Forecaster"));
document.getElementById("btnTaskNTP")       .addEventListener("click", () => toggleTask("NTP"));
document.getElementById("btnTaskTVOC")      .addEventListener("click", () => toggleTask("TVOC"));

// ── Команды управления внешним блоком STM32 ──────────────────
//
//  HEATER   — принудительный запуск цикла просушки SHT31
//  NRF_REST — сброс nRF905 на STM32
//  REST     — полный перезапуск STM32 (данные прервутся ~15 сек)
//
//  Команда ставится в очередь на ESP32, отправляется через nRF905
//  задачей taskNRF905Tx, не мешая приёму погодных пакетов.

const EXT_CMD_LABELS = {
    "HEATER":   "запустить цикл просушки",
    "NRF_REST": "сбросить nRF905 на STM32",
    "REST":     "перезапустить STM32"
};

function sendExtCmd(cmd) {
    const label = EXT_CMD_LABELS[cmd] || cmd;
    if (!confirm(`Подтвердите: ${label}?`)) return;

    const resultEl = document.getElementById("result");
    resultEl.innerText = `Отправка команды ${cmd}...`;

    fetch("/sendCommand", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: "cmd=" + encodeURIComponent(cmd)
    })
    .then(r => r.text())
    .then(text => {
        resultEl.innerText = text;
        // Сбрасываем сообщение через 5 секунд
        setTimeout(() => { resultEl.innerText = ""; }, 5000);
    })
    .catch(err => {
        resultEl.innerText = "Ошибка: " + err;
    });
}

// ── Системная информация (uptime, RAM, RSSI) ─────────────────

function loadSystemInfo() {
    fetch("/sysinfo")
        .then(r => r.text())
        .then(text => { document.getElementById("systemInfo").innerText = text; })
        .catch(err => console.error("Ошибка sysinfo:", err));
}

// ── Статус датчиков I2C ───────────────────────────────────────

function loadBMEInfo() {
    fetch("/bmeinfo")
        .then(r => r.text())
        .then(text => { document.getElementById("bmeStatus").innerText = text; })
        .catch(err => console.error("Ошибка bmeinfo:", err));
}

// ── Статус nRF905 ─────────────────────────────────────────────

function loadNRF905Info() {
    fetch("/nrf905Status")
        .then(r => r.text())
        .then(text => { document.getElementById("nrf905Status").innerText = text; })
        .catch(err => console.error("Ошибка nrf905Status:", err));
}

// ── Отправка настроек nRF905 (канал, диапазон, мощность) ──────

function sendNRFConfig() {
    const params = new URLSearchParams({
        channel: document.getElementById("channel").value,
        band:    document.querySelector("input[name='band']:checked").value,
        power:   document.getElementById("power").value
    });
    fetch("/setNRF905", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: params.toString()
    })
    .then(r => r.text())
    .then(text => { document.getElementById("result").innerText = text; })
    .catch(err => { document.getElementById("result").innerText = "Ошибка: " + err; });
}

// ── Аппаратный сброс nRF905 (ESP32) ──────────────────────────

function resetNRF() {
    fetch("/nrfreset", { method: "POST" })
        .then(r => r.text())
        .then(text => { document.getElementById("result").innerText = text; })
        .catch(err => { document.getElementById("result").innerText = "Ошибка: " + err; });
}

// ── Инициализация при загрузке страницы ──────────────────────

window.addEventListener("load", () => {
    fetchTaskStates();
    loadSystemInfo();
    loadBMEInfo();
    loadNRF905Info();

    // Периодическое обновление системной информации
    setInterval(() => {
        loadSystemInfo();
        loadBMEInfo();
        loadNRF905Info();
    }, 5000);
});