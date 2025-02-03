document.getElementById("RestartButton").addEventListener("click", function(e) {
    // Отменяем стандартное поведение формы
    e.preventDefault();
    
    // Отправляем POST-запрос к /restart
    fetch("/restart", { method: "POST" })
      .then(response => response.text())
      .then(data => {
          console.log("Response:", data);
          // Можно показать уведомление или перезагрузить страницу, если нужно:
          // location.reload();
      })
      .catch(error => console.error("Error:", error));
});

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

// Вызываем обновление после загрузки страницы и, например, периодически
window.addEventListener('load', () => {
    loadSystemInfo();
    loadBMEInfo();
    loadNRF905Info();
    
    // Обновляем информацию каждые 10 секунд (по необходимости)
    setInterval(() => {
        loadSystemInfo();
        loadBMEInfo();
    }, 10000);
});