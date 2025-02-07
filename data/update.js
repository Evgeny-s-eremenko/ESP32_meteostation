document.getElementById('updatebtn').addEventListener('click', function(event) {
    event.preventDefault(); // Отменяем стандартное поведение отправки формы

    let formData = new FormData(document.querySelector('form')); // Собираем данные формы

    document.getElementById('result').innerText = 'Обновление началось...'; // Сообщаем пользователю

    fetch('/update', {
        method: 'POST',
        body: formData
    })
    .then(response => response.text())
    .then(status => {
        document.getElementById('result').innerText = 'Статус: ' + status;
    })
    .catch(err => {
        document.getElementById('result').innerText = 'Ошибка: ' + err;
    });
});