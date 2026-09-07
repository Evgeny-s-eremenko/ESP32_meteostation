# MCP-сервер для ESP32 метеостанции

Локальный MCP-сервер для диагностики и управления ESP32 метеостанцией через HTTP API.

## Требования

- Python 3.10+
-pip (или uv/pipx)

## Установка

```bash
cd mcp_server
pip install -r requirements.txt
```

## Конфигурация

Сервер читает настройки из переменных окружения:

| Переменная | Описание | Обязательна |
|------------|----------|-------------|
| `METEOSTATION_URL` | Адрес станции, например `http://192.168.1.100` | Да |
| `METEOSTATION_USER` | Логин Basic Auth | Да |
| `METEOSTATION_PASSWORD` | Пароль Basic Auth | Да |
| `METEOSTATION_TIMEOUT` | Таймаут HTTP-запросов (сек, по умолчанию 10) | Нет |

### Откуда берутся учётные данные

Логин и пароль совпадают с теми, что заданы в `src/secrets.h` проекта метеостанции
(определения `SECRET_HTTP_USER` / `SECRET_HTTP_PASSWORD`).
Эти же данные используются для доступа к административной панели метеостанции в браузере.

**Ни пароль, ни логин не хранятся в коде MCP-сервера.** Они передаются только через
переменные окружения и не возвращаются в ответах инструментов.

## Запуск

```bash
# ручной запуск (для отладки)
METEOSTATION_URL=http://192.168.1.100 \
METEOSTATION_USER=evgen \
METEOSTATION_PASSWORD=secret \
python server.py
```

## Регистрация в OpenCode

Добавьте в `~/.config/opencode/opencode.jsonc`:

```json
{
  "mcp": {
    "meteostation": {
      "type": "local",
      "command": ["python", "E:/evgen/Arduino/platformio/ESP32_meteostation/mcp_server/server.py"],
      "enabled": true,
      "environment": {
        "METEOSTATION_URL": "http://192.168.1.100",
        "METEOSTATION_USER": "evgen",
        "METEOSTATION_PASSWORD": "your_password_here"
      }
    }
  },
  "permission": {
    "meteostation_*": "ask",
    "meteostation_get_*": "allow"
  }
}
```

Замените `METEOSTATION_URL`, `METEOSTATION_USER` и `METEOSTATION_PASSWORD` на реальные значения.

## Инструменты

### Read-only (разрешены всегда)

| Инструмент | Описание |
|------------|----------|
| `meteostation_get_graph_data` | Текущие метеоданные (JSON) |
| `meteostation_get_tasks_state` | Состояния FreeRTOS-задач |
| `meteostation_get_system_info` | Системная информация ESP32 |
| `meteostation_get_sensor_info` | Диагностика I2C-датчиков |
| `meteostation_get_radio_status` | Состояние nRF905 |
| `meteostation_get_settings` | Текущие настройки NVS |

### Mutating (требуют подтверждения)

| Инструмент | Описание |
|------------|----------|
| `meteostation_set_settings` | Изменение системных настроек |
| `meteostation_toggle_task` | Включение/выключение задачи |
| `meteostation_send_command` | Команда удалённому STM32 |
| `meteostation_set_nrf905` | Настройка nRF905 |
| `meteostation_reset_nrf905` | Сброс локального nRF905 |
| `meteostation_reset_i2c` | Сброс шины I2C |
| `meteostation_reset_nvs` | Сброс NVS до заводских |
| `meteostation_restart` | Перезагрузка ESP32 |
| `meteostation_upload_firmware` | OTA-загрузка прошивки |
| `meteostation_upload_filesystem` | OTA-загрузка файловой системы |
