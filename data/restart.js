document.getElementById("RestartButton").addEventListener("click", function(e) {
    // Отменяем стандартное поведение формы
    e.preventDefault();
    
    // Отправляем POST-запрос к /restart
    fetch("/restart", { method: "POST" })
      .then(response => response.text())
      .then(data => {
          document.getElementById('result').innerText = data;
          // Можно показать уведомление или перезагрузить страницу, если нужно:
          // location.reload();
      })
      .catch(error => console.error("Error:", error));
});

const socket = new WebSocket("ws://" + location.host + ":81");

socket.onmessage = function (event) {
  try {
    const data = JSON.parse(event.data);
    updateButtonState("TaskWebServer", data.webServer);
    updateButtonState("TaskNRF905", data.nRF905);
    updateButtonState("TaskCO2", data.CO2);
    updateButtonState("TaskNextion", data.nextion);
    updateButtonState("TaskBMP280", data.BMP280);
    updateButtonState("TaskInfluxDB", data.InfluxDB);
    updateButtonState("TaskForecaster", data.Forecaster);
    updateButtonState("TaskNTP", data.NTP);
  } catch (error) {
    console.error("Ошибка парсинга JSON:", error);
  }
};

async function fetchTaskStates() {
  try {
    const response = await fetch('/getTasksState');
    if (!response.ok) throw new Error(`Ошибка: ${response.status}`);
    
    const data = await response.json();
    updateButtonState("TaskWebServer", data.webServer);
    updateButtonState("TaskNRF905", data.nRF905);
    updateButtonState("TaskCO2", data.CO2);
    updateButtonState("TaskNextion", data.nextion);
    updateButtonState("TaskBMP280", data.BMP280);
    updateButtonState("TaskInfluxDB", data.InfluxDB);
    updateButtonState("TaskForecaster", data.Forecaster);
    updateButtonState("TaskNTP", data.NTP);
  } catch (error) {
    console.error("Ошибка получения данных:", error);
  }
}

function updateButtonState(task, isRunning) {
  const btn = document.getElementById("btn" + task);
  if (!btn) return; // Если кнопки нет, ничего не делаем

  if (isRunning) {
    btn.classList.remove("stopped");
    btn.classList.add("running");
    btn.textContent = btn.textContent.replace("Stopped", "Running");
  } else {
    btn.classList.remove("running");
    btn.classList.add("stopped");
    btn.textContent = btn.textContent.replace("Running", "Stopped");
  }
}

function toggleTask(task) {
  fetch("/toggleTask", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: "task=" + task
  }).catch(error => console.error("Ошибка:", error));
}

  document.getElementById("btnTaskWebServer").addEventListener("click", () => toggleTask("webServer"));
  document.getElementById("btnTaskNRF905").addEventListener("click", () => toggleTask("nRF905"));
  document.getElementById("btnTaskCO2").addEventListener("click", () => toggleTask("CO2"));
  document.getElementById("btnTaskNextion").addEventListener("click", () => toggleTask("nextion"));
  document.getElementById("btnTaskBMP280").addEventListener("click", () => toggleTask("BMP280"));
  document.getElementById("btnTaskInfluxDB").addEventListener("click", () => toggleTask("InfluxDB"));
  document.getElementById("btnTaskForecaster").addEventListener("click", () => toggleTask("Forecaster"));
  document.getElementById("btnTaskNTP").addEventListener("click", () => toggleTask("NTP"));

function loadSystemInfo() {
    fetch('/sysinfo')
        .then(response => response.text())
        .then(data => {
            document.getElementById('systemInfo').innerText = data;
        })
        .catch(err => console.error('Error fetching system info:', err));
}

function loadBMEInfo() {
    fetch('/bmeinfo')
        .then(response => response.text())
        .then(data => {
            document.getElementById('bmeStatus').innerText = data;
        })
        .catch(err => console.error('Error fetching BME280 info:', err));
}

function loadNRF905Info() {
    fetch("/nrf905Status")
        .then(response => response.text())
        .then(data => {
            document.getElementById("nrf905Status").innerText = data;
        });
}
function sendNRFConfig() {
    // Получаем значения из формы
    var channel = document.getElementById('channel').value;
    var band = document.querySelector('input[name="band"]:checked').value;
    var power = document.getElementById('power').value;

    // Формируем параметры запроса
    var params = new URLSearchParams();
    params.append('channel', channel);
    params.append('band', band);
    params.append('power', power);

    // Отправляем AJAX-запрос методом POST
    fetch('/setNRF905', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/x-www-form-urlencoded'
      },
      body: params.toString()
    })
    .then(response => response.text())
    .then(text => {
      document.getElementById('result').innerText = text;
    })
    .catch(err => {
      document.getElementById('result').innerText = 'Ошибка: ' + err;
    });
}

function resetNRF(){
    fetch('/nrfreset', {
      method: 'POST'})
      .then(response => response.text())
      .then(text => {document.getElementById('result').innerText = text;
      })
      .catch(err => {
        document.getElementById('result').innerText = 'Ошибка: ' + err;
      });
}
// Вызываем обновление после загрузки страницы и, например, периодически
window.addEventListener('load', () => {
    fetchTaskStates();
    loadSystemInfo();
    loadBMEInfo();
    loadNRF905Info();
    
    // Обновляем информацию каждые 10 секунд (по необходимости)
    setInterval(() => {
        loadSystemInfo();
        loadBMEInfo();
        loadNRF905Info();
    }, 5000);
});