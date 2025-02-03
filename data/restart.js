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