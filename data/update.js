document.getElementById('updatebtn').addEventListener('click', function(event) {
    event.preventDefault(); // Отменяем стандартное поведение отправки формы

    let formData = new FormData(document.querySelector('form')); // Собираем данные формы

    document.getElementById('result').innerText = 'Обновление началось...'; // Сообщаем пользователю

    fetch('/update', {
        method: 'POST',
        body: formData
    })
    .then(response => {
        console.log('HTTP Status:', response.status);
        return response.text();
    })
    .then(status => {
        console.log('Update status:', status);
        document.getElementById('result').innerText = 'Статус: ' + status;

        if (status === 'OK') {
            setTimeout(() => {
                window.location.href = '/';
            }, 500);
        }
    })
    .catch(err => {
        console.error('Fetch error:', err);
        document.getElementById('result').innerText = 'Ошибка: ' + err;
    });
});