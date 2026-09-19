# План реорганизации агентской инфраструктуры

Статус: утверждено на этапе Decision Gates, не реализовано.

## 1. Исходное состояние

- OpenCode: 1.18.31, Windows, запуск из терминала VSCode.
- Проектный конфиг: `opencode.json`, сейчас содержит только reference `@stm32`.
- Глобальный конфиг: `C:\Users\evgen\.config\opencode\opencode.jsonc`.
- Проектный агент: `.opencode/agents/esp32.md`.
- Проектные commands отсутствуют.
- Project skill: `.agents/skills/embedded-systems/`.
- `skills-lock.json` находится в корне проекта и содержит metadata происхождения skill.
- Project MCP implementation: `mcp_server/`.
- Все три MCP сейчас зарегистрированы глобально: `meteostation`, `playwright`, `context7`.
- `.mimocode/` больше не используется и является локальным историческим артефактом.
- `.playwright-mcp/` содержит runtime/generated artifacts в проекте.
- `AGENTS.md` ссылается на полный `README.MD` за HTTP API, NVS и структурой проекта.
- `.gitignore` игнорирует `.opencode/`, `.agents/` и `skills-lock.json`.
- В глобальном MCP-конфиге обнаружены credentials `meteostation`; их значения не должны попасть в новый план или tracked files.

## 2. Проверенные возможности OpenCode 1.18.31

- Есть встроенные агенты `build`, `plan`, `explore`, `general`.
- Поддерживаются custom agents.
- Поддерживаются custom commands.
- Поддерживаются project/global MCP через merged config.
- Поддерживаются path-specific permissions.
- Skills фактически обнаруживаются из `.agents/skills` и поддерживаются из `.opencode/skills`.
- `opencode debug config` подтверждает объединение global и project config, приоритет project config.
- Встроенный `plan` agent уже ограничен правом редактирования `.opencode\\plans\\*.md`, но для проекта выбран отдельный planner.
- Штатного plan-specific auto-open в VSCode не обнаружено.
- Штатной команды сохранения плана в файл не обнаружено.

## 3. Принятые решения D1-D11

### D1. Планы

Выбран отдельный project agent `planner` и project command `/plan`.

Planner должен:

- анализировать проект;
- создавать и изменять только `.opencode/plans/*.md`;
- не изменять исходный код;
- не изменять конфигурацию проекта;
- использовать строгий шаблон плана;
- после фактического создания файла вывести отдельной строкой точный относительный путь:

```text
PLAN: .opencode/plans/YYYY-MM-DD-name.md
```

Путь должен соответствовать реально созданному файлу, а не предположительному имени.

### D2. Открытие планов в VSCode

Автоматического открытия не будет.

Не создавать:

- `/open-plan`;
- PowerShell-скрипт открытия;
- VSCode Task для открытия;
- OpenCode plugin для открытия.

Пользователь открывает файл вручную по строке `PLAN:`.

### D3. Физическое расположение project MCP

Переместить реализацию метеостанционного MCP в:

```text
.mcp/meteostation/
├── server.py
├── requirements.txt
└── README.md
```

`.mcp/` является организационным каталогом. OpenCode не должен рассматриваться как автоматически обнаруживающий MCP из `.mcp/`; регистрация остается явной в project `opencode.json`.

### D4. Область MCP

- `meteostation`: project MCP, зарегистрирован только в project `opencode.json`.
- `context7`: global MCP, зарегистрирован только в global `opencode.jsonc`.
- `playwright`: global MCP, зарегистрирован только в global `opencode.jsonc`, но отключен по умолчанию.
- `.playwright-mcp/` вывести из project root и перенести в:
  `C:\Users\evgen\.config\opencode\.mcp\playwright-mcp\`.
- Все глобальные MCP физически организуются в:
  `C:\Users\evgen\.config\opencode\.mcp\`.

### D5. Skills

Перенести единственную копию project skill из:

```text
.agents/skills/embedded-systems/
```

в:

```text
.opencode/skills/embedded-systems/
```

Не создавать вторую копию и не использовать `.skills/`.

### D5a. `skills-lock.json`

Оставить `skills-lock.json` в корне проекта.

Он является metadata/lock-файлом происхождения skill, а не конфигурацией OpenCode. В `AGENTS.md` явно описать, что OpenCode не использует его для discovery или permissions.

### D6. Форматы конфигов

- Project: `opencode.json`.
- Global: `C:\Users\evgen\.config\opencode\opencode.jsonc`.

OpenCode deep-merges конфигурации. Project config имеет более высокий приоритет в конфликтующих ключах.

### D7. Git

Отслеживать проектный `.opencode/`, исключив:

- `node_modules`;
- generated files;
- runtime files;
- local secrets.

Отслеживать `.opencode/plans/`, project skills, project commands и agents.

`skills-lock.json` отслеживать как metadata.

Project `.mcp/` отслеживать, кроме локальных secrets и runtime artifacts.

`.mimocode/` остается локально, не удаляется и продолжает игнорироваться Git.

### D8. `AGENTS.md`

Использовать компактный гибрид:

- полное дерево значимых файлов и каталогов проекта;
- часто используемые HTTP API;
- WebSocket paths;
- NVS namespaces и operational parameters;
- правила planner;
- project/global scope для MCP и skills;
- source-of-truth rules;
- запрет чтения полного README без необходимости.

Большие подробности остаются в `README.MD` и skills.

### D9. `README.MD`

- Оставить README человекоориентированным источником подробной документации.
- Сохранить подробные API/NVS/архитектурные разделы.
- Сократить дерево проекта в README.
- Полное значимое дерево для агента разместить в `AGENTS.md`.
- Для фактического поведения считать source of truth исходный код, а не README.

### D10. Secrets

Использовать локальный secrets-файл внутри project MCP, вне Git:

```text
.mcp/meteostation/.env
```

Tracked OpenCode configs не должны содержать пароли, токены и другие credentials.

Существующие `meteostation` credentials удалить из global config. Перед реализацией проверить безопасный способ передачи значений из локального файла в MCP environment, не выводя содержимое secrets.

### D11. Контроль согласованности

Создать оба механизма:

```text
scripts/verify-agent-structure.ps1
.opencode/commands/verify.md
```

PowerShell-скрипт выполняет детерминированные read-only проверки. `/verify` предоставляет интерактивный интерфейс, но не исправляет ошибки автоматически.

Проверять структуру, scope MCP, skill frontmatter, расположение plans, permissions planner, Git/secrets boundaries и наличие legacy paths. Не пытаться автоматически сравнивать весь HTTP API с C++-кодом.

## 4. Целевая структура

```text
ESP32_meteostation/
├── .mcp/
│   └── meteostation/
│       ├── .env                  # локальный, не в Git
│       ├── server.py
│       ├── requirements.txt
│       └── README.md
├── .opencode/
│   ├── agents/
│   │   ├── esp32.md
│   │   └── planner.md
│   ├── commands/
│   │   ├── plan.md
│   │   └── verify.md
│   ├── plans/
│   │   └── agent-infrastructure-migration.md
│   └── skills/
│       └── embedded-systems/
│           ├── SKILL.md
│           └── references/
├── data/
├── include/
├── lib/
├── mcp_server/                   # удалить после переноса и проверки ссылок
├── scripts/
│   └── verify-agent-structure.ps1
├── src/
├── AGENTS.md
├── README.MD
├── opencode.json
├── platformio.ini
├── secrets.h.example
└── skills-lock.json
```

Глобальная структура:

```text
C:\Users\evgen\.config\opencode\
├── AGENTS.md
├── .mcp/
│   ├── context7-mcp/             # только если нужны физические локальные данные
│   └── playwright-mcp/            # перенесенные runtime/generated artifacts
├── opencode.jsonc
├── agents/
├── commands/
└── skills/
```

`context7` является global MCP и регистрируется в global config. Если для remote MCP не требуется физический каталог, не создавать искусственную реализацию в `.mcp/context7-mcp/`; каталог `.mcp/` предназначен для фактически хранимых global MCP files и artifacts.

## 5. Порядок реализации

1. Сохранить текущее состояние и проверить независящие от миграции внешние ссылки. Не изменять firmware source.
2. Создать/обновить Git rules для отслеживания `.opencode/`, `.mcp/`, `skills-lock.json` и исключения secrets/runtime files, сохранив `.mimocode/` в ignore.
3. Переместить project skill в `.opencode/skills/embedded-systems/`, сохранив references и единственную копию.
4. Переместить MCP implementation из `mcp_server/` в `.mcp/meteostation/` и обновить документацию/пути.
5. Создать `.mcp/meteostation/.env` только локально и настроить его ignore rule. Не записывать реальные значения в tracked files.
6. Перенести `meteostation` registration и project permissions из global config в project `opencode.json`.
7. Оставить `context7` в global config. Оставить `playwright` в global config с `enabled: false`; проверить механизм явного включения.
8. Удалить project `.playwright-mcp/` из project root путем переноса в `C:\Users\evgen\.config\opencode\.mcp\playwright-mcp\`. Не включать runtime artifacts в Git.
9. Создать ограниченный project agent `planner` с edit permission только для `.opencode/plans/*.md`, без прав изменения source/config. Настройку проверять через `opencode debug agent planner`.
10. Создать project command `/plan`, которая запускает `planner`, требует фактического создания plan-файла и отдельной строки `PLAN: <точный относительный путь>`.
11. Создать `scripts/verify-agent-structure.ps1` и read-only command `/verify`.
12. Обновить `AGENTS.md`: добавить полное значимое дерево, planner workflow, MCP/skills scope, HTTP API, WebSocket, NVS и source-of-truth rules; убрать требование читать полный README для базовой информации.
13. Сократить дерево в `README.MD`, сохранив подробную человекоориентированную документацию и указав актуальные пути.
14. Проверить `skills-lock.json`: оставить в корне как metadata, не использовать как OpenCode config.
15. Выполнить диагностику OpenCode, проверку JSON/JSONC, проверку skills, agents, MCP и Git. Ничего не исправлять автоматически скриптом `/verify`.

## 6. Риски и меры

### Риск: MCP не запускается после перемещения

Мера: проверить `cwd`, относительные пути, Python dependencies и `opencode mcp list` до удаления старого расположения.

### Риск: secrets попадут в Git

Мера: отдельное правило для `.mcp/meteostation/.env`, проверка `git status`, `git diff --cached` и поиск секретоподобных значений без вывода содержимого.

### Риск: planner получит лишние права

Мера: path-specific `edit` permissions, запрет source/config paths, проверка через `opencode debug agent planner`.

### Риск: дублирование skill

Мера: после переноса оставить только `.opencode/skills/embedded-systems/`, удалить старый project copy после проверки.

### Риск: проектный MCP останется глобальным

Мера: проверить resolved config и явно убедиться, что `meteostation` отсутствует в global config.

### Риск: Playwright добавляет контекст по умолчанию

Мера: global registration с `enabled: false`; отдельно проверить явное включение перед использованием.

### Риск: README и AGENTS.md расходятся

Мера: source-of-truth matrix в `AGENTS.md`; README остается подробным документом, а не runtime-конфигом.

### Риск: исторический Mimocode будет случайно удален

Мера: не удалять `.mimocode/`; оставить локально и в `.gitignore`.

## 7. Критерии успешного завершения

- `opencode --version` остается 1.18.31 или изменения версии отдельно согласованы.
- `opencode debug config` проходит без ошибок.
- `opencode debug agent planner` показывает project planner.
- Planner может редактировать только `.opencode/plans/*.md`.
- Planner не может редактировать `src/`, `data/`, `AGENTS.md`, `README.MD`, `opencode.json` или `.mcp/`.
- `/plan` создает фактический файл в `.opencode/plans/`.
- Ответ `/plan` содержит отдельную строку вида:

```text
PLAN: .opencode/plans/YYYY-MM-DD-name.md
```

- `meteostation` зарегистрирован только в project config.
- `context7` зарегистрирован только глобально.
- `playwright` зарегистрирован глобально и отключен по умолчанию.
- Project MCP физически находится в `.mcp/meteostation/`.
- `.playwright-mcp/` отсутствует в project root, а runtime artifacts находятся вне проекта.
- Единственная копия project skill находится в `.opencode/skills/embedded-systems/`.
- `skills-lock.json` остается в корне как metadata.
- `.mimocode/` остается локально и не удаляется.
- Secrets отсутствуют в tracked JSON/JSONC и tracked project files.
- `scripts/verify-agent-structure.ps1` проходит все структурные проверки.
- `/verify` не изменяет файлы и корректно сообщает об ошибках.
- `AGENTS.md` содержит полное значимое дерево и operational knowledge.
- Дерево `README.MD` сокращено и не противоречит фактической структуре.
- Firmware source, `platformio.ini` и функциональная логика ESP32 не изменены.
- Git показывает только ожидаемые изменения инфраструктуры.

## 8. Ограничения плана

- План не включает изменение firmware functionality.
- План не включает автоматическое открытие plan-файла в VSCode.
- План не включает `/open-plan`.
- План не включает PowerShell-скрипт открытия плана.
- План не включает VSCode Task для открытия плана.
- План не включает OpenCode plugin.
- План не удаляет локальный `.mimocode/`.
- План не добавляет вторую копию project skill.
- До отдельного подтверждения пользователя реализация не начинается.
