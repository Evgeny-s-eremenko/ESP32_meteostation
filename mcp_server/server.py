"""
MCP-сервер для взаимодействия с ESP32 метеостанцией по HTTP API.

Конфигурация через переменные окружения:
  METEOSTATION_URL      — адрес станции (например http://192.168.1.100)
  METEOSTATION_USER     — логин Basic Auth
  METEOSTATION_PASSWORD — пароль Basic Auth
  METEOSTATION_TIMEOUT  — таймаут HTTP-запросов в секундах (по умолчанию 10)
"""

from __future__ import annotations

import os
import sys
import json
import mimetypes
from pathlib import Path
from typing import Any

import httpx
from mcp.server.mcpserver import MCPServer

# ─── Конфигурация ────────────────────────────────────────────────────────────

MCP_SERVER_NAME = "meteostation"


def _get_config() -> dict[str, Any]:
    url = os.environ.get("METEOSTATION_URL", "").rstrip("/")
    user = os.environ.get("METEOSTATION_USER", "")
    password = os.environ.get("METEOSTATION_PASSWORD", "")
    timeout = float(os.environ.get("METEOSTATION_TIMEOUT", "10"))

    if not url:
        print("ОШИБКА: METEOSTATION_URL не задан", file=sys.stderr)
        sys.exit(1)

    return {"url": url, "user": user, "password": password, "timeout": timeout}


mcp = MCPServer(MCP_SERVER_NAME)


# ─── Вспомогательные функции ─────────────────────────────────────────────────

def _client(cfg: dict[str, Any]) -> httpx.Client:
    return httpx.Client(
        base_url=cfg["url"],
        auth=(cfg["user"], cfg["password"]) if cfg["user"] else None,
        timeout=cfg["timeout"],
    )


def _get(cfg: dict[str, Any], path: str) -> httpx.Response:
    with _client(cfg) as client:
        resp = client.get(path)
        resp.raise_for_status()
        return resp


def _post(cfg: dict[str, Any], path: str, data: dict | None = None) -> httpx.Response:
    with _client(cfg) as client:
        resp = client.post(path, data=data)
        resp.raise_for_status()
        return resp


def _post_multipart(
    cfg: dict[str, Any], path: str, file_path: Path, field_name: str = "update"
) -> httpx.Response:
    mime, _ = mimetypes.guess_type(str(file_path))
    with _client(cfg) as client:
        with open(file_path, "rb") as f:
            resp = client.post(
                path,
                files={field_name: (file_path.name, f, mime or "application/octet-stream")},
            )
            resp.raise_for_status()
            return resp


def _format_error(e: Exception) -> str:
    if isinstance(e, httpx.HTTPStatusError):
        return f"HTTP {e.response.status_code}: {e.response.text}"
    return str(e)


# ─── Read-only tools ─────────────────────────────────────────────────────────


@mcp.tool()
def meteostation_get_graph_data() -> str:
    """Получить текущие метеоданные (температура, влажность, давление, CO2, TVOC, PM2.5, PM10, UV, LUX и т.д.).

    Возвращает JSON со всеми измерениями с уличного и внутреннего датчиков.
    """
    cfg = _get_config()
    try:
        resp = _get(cfg, "/graph-data")
        return resp.text
    except Exception as e:
        return f"Ошибка: {_format_error(e)}"


@mcp.tool()
def meteostation_get_tasks_state() -> str:
    """Получить состояния FreeRTOS-задач (включена/выключена каждая задача).

    Возвращает JSON: {"nRF905": bool, "CO2": bool, "nextion": bool, ...}
    """
    cfg = _get_config()
    try:
        resp = _get(cfg, "/getTasksState")
        return resp.text
    except Exception as e:
        return f"Ошибка: {_format_error(e)}"


@mcp.tool()
def meteostation_get_system_info() -> str:
    """Получить системную информацию: Chip Model,.heap, uptime, RSSI, IP, стек.

    Возвращает текстовый отчёт о состоянии ESP32.
    """
    cfg = _get_config()
    try:
        resp = _get(cfg, "/sysinfo")
        return resp.text
    except Exception as e:
        return f"Ошибка: {_format_error(e)}"


@mcp.tool()
def meteostation_get_sensor_info() -> str:
    """Получить диагностику I2C-датчиков: BME280, ENS160, AHT20 и счётчик сбросов I2C.

    Возвращает текстовый отчёт о состоянии датчиков и шины I2C.
    """
    cfg = _get_config()
    try:
        resp = _get(cfg, "/bmeinfo")
        return resp.text
    except Exception as e:
        return f"Ошибка: {_format_error(e)}"


@mcp.tool()
def meteostation_get_radio_status() -> str:
    """Получить состояние и параметры радиомодуля nRF905: регистры, канал, частота, мощность, статистика приёма.

    Возвращает текстовый отчёт о состоянии радиоканала.
    """
    cfg = _get_config()
    try:
        resp = _get(cfg, "/nrf905Status")
        return resp.text
    except Exception as e:
        return f"Ошибка: {_format_error(e)}"


@mcp.tool()
def meteostation_get_settings() -> str:
    """Получить текущие сохранённые настройки станции из NVS.

    Возвращает JSON с настройками WiFi, HTTP-авторизации, статического IP,
    InfluxDB, NTP, координат, поправки температуры и высоты.
    Пароли маскируются звёздочками.
    """
    cfg = _get_config()
    try:
        resp = _get(cfg, "/getSettings")
        return resp.text
    except Exception as e:
        return f"Ошибка: {_format_error(e)}"


# ─── Mutating tools ──────────────────────────────────────────────────────────


@mcp.tool()
def meteostation_set_settings(
    wifi_ssid: str | None = None,
    wifi_pass: str | None = None,
    http_user: str | None = None,
    http_pass: str | None = None,
    use_static_ip: int | None = None,
    static_ip: str | None = None,
    static_gateway: str | None = None,
    static_subnet: str | None = None,
    static_dns: str | None = None,
    influx_host: str | None = None,
    influx_port: int | None = None,
    influx_db: str | None = None,
    ntp_server: str | None = None,
    latitude: float | None = None,
    longitude: float | None = None,
    tz_offset: int | None = None,
    tz_sec: int | None = None,
    tCorr: float | None = None,
    altitude_m: float | None = None,
) -> str:
    """Изменить системные настройки станции (WiFi, InfluxDB, NTP, координаты и др.).

    Настройки сохраняются в NVS. Для применения изменений требуется перезагрузка станции.
    Если параметр не передан, он не изменяется. Пароли передаются в открытом виде
    и сохраняются на станции; передача \"****\" игнорируется (защита от случайной перезаписи).
    """
    cfg = _get_config()
    params: dict[str, str] = {}
    if wifi_ssid is not None:
        params["wifi_ssid"] = wifi_ssid
    if wifi_pass is not None:
        params["wifi_pass"] = wifi_pass
    if http_user is not None:
        params["http_user"] = http_user
    if http_pass is not None:
        params["http_pass"] = http_pass
    if use_static_ip is not None:
        params["use_static_ip"] = str(use_static_ip)
    if static_ip is not None:
        params["static_ip"] = static_ip
    if static_gateway is not None:
        params["static_gateway"] = static_gateway
    if static_subnet is not None:
        params["static_subnet"] = static_subnet
    if static_dns is not None:
        params["static_dns"] = static_dns
    if influx_host is not None:
        params["influx_host"] = influx_host
    if influx_port is not None:
        params["influx_port"] = str(influx_port)
    if influx_db is not None:
        params["influx_db"] = influx_db
    if ntp_server is not None:
        params["ntp_server"] = ntp_server
    if latitude is not None:
        params["latitude"] = str(latitude)
    if longitude is not None:
        params["longitude"] = str(longitude)
    if tz_offset is not None:
        params["tz_offset"] = str(tz_offset)
    if tz_sec is not None:
        params["tz_sec"] = str(tz_sec)
    if tCorr is not None:
        params["tCorr"] = str(tCorr)
    if altitude_m is not None:
        params["altitude_m"] = str(altitude_m)

    if not params:
        return "Ошибка: не указан ни один параметр для изменения"

    try:
        resp = _post(cfg, "/setSettings", data=params)
        return resp.text
    except Exception as e:
        return f"Ошибка: {_format_error(e)}"


@mcp.tool()
def meteostation_toggle_task(task: str) -> str:
    """Переключить состояние FreeRTOS-задачи (включить/выключить).

    Допустимые значения task: nRF905, CO2, nextion, BMP280, InfluxDB, Forecaster, NTP, TVOC.
    Если задача активна — она приостанавливается, если остановлена — возобновляется.
    """
    cfg = _get_config()
    valid_tasks = {"nRF905", "CO2", "nextion", "BMP280", "InfluxDB", "Forecaster", "NTP", "TVOC"}
    if task not in valid_tasks:
        return f"Ошибка: недопустимое имя задачи '{task}'. Допустимые: {', '.join(sorted(valid_tasks))}"
    try:
        resp = _post(cfg, "/toggleTask", data={"task": task})
        return resp.text
    except Exception as e:
        return f"Ошибка: {_format_error(e)}"


@mcp.tool()
def meteostation_send_command(cmd: str) -> str:
    """Отправить команду удалённому STM32-модулю через nRF905.

    Допустимые команды:
      HEATER  — включение/выключение нагревателя датчика
      NRF_REST — сброс радиомодуля nRF905 на удалённом узле
      REST    — полный перезапуск STM32
    """
    cfg = _get_config()
    valid_cmds = {"HEATER", "NRF_REST", "REST"}
    if cmd not in valid_cmds:
        return f"Ошибка: недопустимая команда '{cmd}'. Допустимые: {', '.join(sorted(valid_cmds))}"
    try:
        resp = _post(cfg, "/sendCommand", data={"cmd": cmd})
        return resp.text
    except Exception as e:
        return f"Ошибка: {_format_error(e)}"


@mcp.tool()
def meteostation_set_nrf905(channel: int, band: str, power: str) -> str:
    """Настроить параметры радиомодуля nRF905 и сохранить в NVS.

    Параметры:
      channel — номер канала (0-255)
      band    — частотный диапазон: \"true\" (868/915 МГц) или \"false\" (430 МГц)
      power   — мощность передатчика: \"TransmitPowerm10dBm\", \"TransmitPowerm2dBm\",
                \"TransmitPower6dBm\", \"TransmitPower10dBm\"
    """
    cfg = _get_config()
    valid_bands = {"true", "false"}
    valid_powers = {"TransmitPowerm10dBm", "TransmitPowerm2dBm", "TransmitPower6dBm", "TransmitPower10dBm"}
    errors = []
    if band not in valid_bands:
        errors.append(f"band: '{band}' (допустимые: true, false)")
    if power not in valid_powers:
        errors.append(f"power: '{power}' (допустимые: {', '.join(sorted(valid_powers))})")
    if errors:
        return "Ошибка: " + "; ".join(errors)
    try:
        resp = _post(cfg, "/setNRF905", data={"channel": str(channel), "band": band, "power": power})
        return resp.text
    except Exception as e:
        return f"Ошибка: {_format_error(e)}"


@mcp.tool()
def meteostation_reset_nrf905() -> str:
    """Выполнить аппаратный сброс локального радиомодуля nRF905."""
    cfg = _get_config()
    try:
        resp = _post(cfg, "/nrfreset")
        return resp.text
    except Exception as e:
        return f"Ошибка: {_format_error(e)}"


@mcp.tool()
def meteostation_reset_i2c() -> str:
    """Выполнить программный сброс шины I2C (восстановление при зависании устройств)."""
    cfg = _get_config()
    try:
        resp = _post(cfg, "/resetI2C")
        return resp.text
    except Exception as e:
        return f"Ошибка: {_format_error(e)}"


@mcp.tool()
def meteostation_reset_nvs() -> str:
    """Сбросить NVS до заводских настроек. Все параметры будут потеряны!"""
    cfg = _get_config()
    try:
        resp = _post(cfg, "/resetNVS")
        return resp.text
    except Exception as e:
        return f"Ошибка: {_format_error(e)}"


@mcp.tool()
def meteostation_restart() -> str:
    """Перезагрузить ESP32. Станция будет недоступна несколько секунд."""
    cfg = _get_config()
    try:
        resp = _post(cfg, "/restart")
        return resp.text
    except Exception as e:
        return f"Ошибка: {_format_error(e)}"


@mcp.tool()
def meteostation_upload_firmware(file_path: str) -> str:
    """Загрузить новую прошивку (firmware.bin) на станцию через OTA.

    Принимает путь к локальному .bin-файлу прошивки ESP32.
    После успешной загрузки станция автоматически перезагружается.
    """
    cfg = _get_config()
    path = Path(file_path)
    if not path.exists():
        return f"Ошибка: файл не найден: {file_path}"
    if not path.suffix.lower() == ".bin":
        return f"Ошибка: допустимы только .bin файлы, получено: {path.suffix}"
    if path.stat().st_size == 0:
        return "Ошибка: файл пуст (0 байт)"
    try:
        resp = _post_multipart(cfg, "/update", path)
        return f"OTA-обновление прошивки завершено: {resp.text}"
    except Exception as e:
        return f"Ошибка OTA: {_format_error(e)}"


@mcp.tool()
def meteostation_upload_filesystem(file_path: str) -> str:
    """Загрузить файловую систему (littlefs.bin) на станцию через OTA.

    Принимает путь к локальному .bin-файлу LittleFS.
    Имя файла должно содержать \"littlefs\" для корректного определения типа.
    После успешной загрузки станция автоматически перезагружается.
    """
    cfg = _get_config()
    path = Path(file_path)
    if not path.exists():
        return f"Ошибка: файл не найден: {file_path}"
    if not path.suffix.lower() == ".bin":
        return f"Ошибка: допустимы только .bin файлы, получено: {path.suffix}"
    if path.stat().st_size == 0:
        return "Ошибка: файл пуст (0 байт)"
    if "littlefs" not in path.name.lower():
        return f"Ошибка: имя файла должно содержать 'littlefs' для загрузки файловой системы, получено: {path.name}"
    try:
        resp = _post_multipart(cfg, "/update", path)
        return f"OTA-обновление файловой системы завершено: {resp.text}"
    except Exception as e:
        return f"Ошибка OTA: {_format_error(e)}"


# ─── Точка входа ─────────────────────────────────────────────────────────────

if __name__ == "__main__":
    mcp.run(transport="stdio")
