# План проверки после реорганизации агентской инфраструктуры

## Цель и область

Проверить, что реализация плана `.opencode/plans/agent-infrastructure-migration.md` завершена без нарушения границ проекта, секретности и функциональности ESP32. Проверка охватывает файловую структуру, Git-границы, project/global OpenCode-конфигурацию, MCP, skill, planner, команды `/plan` и `/verify`, а также отсутствие изменений firmware и PlatformIO-конфигурации.

Проверка является read-only: скрипты и команды не должны автоматически исправлять найденные ошибки. Мутационные MCP-операции и публикация изменений не входят в область этого плана.

## Подтверждённые факты и допущения

### Подтверждено

- План миграции требует переноса project MCP в `.mcp/meteostation/`, project skill в `.opencode/skills/embedded-systems/`, project-регистрации `meteostation`, глобальной регистрации `context7` и отключённого по умолчанию `playwright`.
- Целевые project-файлы включают `.opencode/agents/planner.md`, `.opencode/commands/plan.md`, `.opencode/commands/verify.md` и `scripts/verify-agent-structure.ps1`.
- `skills-lock.json` должен оставаться в корне как metadata, а `.mimocode/` — сохраняться локально и не удаляться.
- Скрипт `scripts/verify-agent-structure.ps1` уже определяет базовые структурные, skill, MCP и secret-pattern проверки, используя Git-индекс и глобальный конфиг.
- Исходники прошивки, `platformio.ini` и функциональная логика ESP32 не должны изменяться.

### Допущения, которые нужно подтвердить проверкой

- Глобальный OpenCode-конфиг доступен по `$env:USERPROFILE\.config\opencode\opencode.jsonc`, а команда `opencode` запускается в окружении проверки.
- Перенесённый MCP действительно запускается из нового расположения и получает значения secrets через локальный `.env` или другой явно разрешённый безопасный механизм.
- Фактические изменения после миграции ограничены ожидаемыми инфраструктурными файлами и не включают незапланированные изменения рабочего дерева.

## Текущее поведение и релевантные файлы

- Источник требований миграции: `.opencode/plans/agent-infrastructure-migration.md`.
- Структурные проверки: `scripts/verify-agent-structure.ps1`; скрипт должен завершаться успешно и не изменять файлы.
- Project OpenCode-конфигурация: `opencode.json`.
- Global OpenCode-конфигурация: `C:\Users\evgen\.config\opencode\opencode.jsonc`.
- Agent/command/skill boundaries: `.opencode/agents/`, `.opencode/commands/`, `.opencode/skills/`.
- Project MCP: `.mcp/meteostation/`.
- Git-правила и tracked-state: `.gitignore`, `git ls-files`, `git status`, `git diff`.
- Project operational rules: `AGENTS.md`.
- Firmware boundary: `src/`, `data/`, `include/`, `lib/`, `platformio.ini`.

## Упорядоченные шаги проверки

1. **Зафиксировать исходное состояние проверки.** Выполнить `git status --short`, получить список tracked-файлов и сохранить только диагностические результаты без вывода содержимого secrets. Убедиться, что работа выполняется из корня проекта.

2. **Проверить структуру и legacy paths.** Запустить `scripts/verify-agent-structure.ps1`. Зафиксировать успешное прохождение всех проверок; при ошибке остановить дальнейшее утверждение успеха и классифицировать каждое сообщение как структурное, конфигурационное или secret-related.

3. **Проверить project-файлы и отсутствие дублирования.** Убедиться в наличии planner, команд `plan`/`verify`, целевого skill и project MCP. Проверить, что отсутствуют `.agents/skills/embedded-systems/`, `mcp_server/server.py` и project `.playwright-mcp/`, а в `.opencode/skills/` находится только одна project-копия skill.

4. **Проверить конфигурации OpenCode.** Выполнить `opencode --version` и `opencode debug config`. Убедиться, что `meteostation` разрешён из project config, отсутствует в global config, `context7` остаётся глобальным, а `playwright` глобален и имеет `enabled: false`. Проверить отсутствие credentials в tracked JSON/JSONC и project-конфигах.

5. **Проверить planner и его permissions.** Выполнить `opencode debug agent planner`. Подтвердить project planner и разрешение редактирования только `.opencode/plans/*.md`; отдельно проверить запрет для `src/`, `data/`, `AGENTS.md`, `README.MD`, `opencode.json`, `.mcp/` и других конфигурационных путей. Не проверять это разрушительным редактированием: использовать отладочный resolved-конфиг и безопасный тестовый сценарий, если он предусмотрен окружением.

6. **Проверить MCP runtime без мутационных операций.** Выполнить `opencode mcp list` и проверить, что `meteostation` разрешается из project scope. При наличии безопасного read-only способа запуска проверить импорт/старт `.mcp/meteostation/server.py`, зависимости и относительные пути. Не вызывать reset, restart, set, upload или send-command операции.

7. **Проверить команды `/verify` и `/plan`.** Запустить `/verify` и убедиться, что он только сообщает результат и не меняет рабочее дерево. В отдельном контролируемом запуске `/plan` проверить фактическое создание markdown-файла в `.opencode/plans/`, формат имени `YYYY-MM-DD-short-kebab-case.md`, обязательные разделы и отдельную строку `PLAN: .opencode/plans/<реальный-путь>`. После теста удалить только специально созданный тестовый план, если это разрешено процедурой проверки, либо явно отметить его как ожидаемый артефакт.

8. **Проверить Git и secrets boundaries.** Сопоставить `git status --short` с ожидаемым набором инфраструктурных изменений. Убедиться, что `.mcp/meteostation/.env` не tracked, `.env.example` при наличии не содержит реальные значения, runtime/generated artifacts не добавлены, `.mimocode/` не удалён, а `skills-lock.json` tracked и находится в корне. Использовать поиск по именам/шаблонам секретов без печати значений.

9. **Проверить документационную согласованность.** Сверить `AGENTS.md` с фактическими путями и правилами scope. Проверить, что `README.MD` остаётся человекоориентированным, его дерево сокращено согласно плану и он не утверждает устаревшие пути. Фактическое поведение считать по исходному коду и конфигурации, а не по README.

10. **Проверить неизменность firmware boundary.** Сравнить изменения в `src/`, `data/`, `include/`, `lib/` и `platformio.ini` с базовым состоянием миграции. При отсутствии изменений выполнить структурную проверку PlatformIO; при наличии изменений остановить инфраструктурную приёмку и вынести их в отдельное согласование. Полную прошивочную сборку выполнять только если фактический diff затрагивает build-relevant файлы.

11. **Сформировать итоговый отчёт.** Разделить результат на пройденные проверки, блокирующие ошибки, предупреждения и ручные проверки. Приложить команды и безопасные диагностические выводы, но не credentials. Успешным считать только состояние, соответствующее всем критериям раздела 7 исходного плана миграции.

## Риски и ограничения

- Глобальный конфиг и пользовательский MCP находятся вне корня проекта; проверка не должна изменять их.
- Нельзя выводить или копировать значения `.env`, `secrets.h` или credentials из global config.
- `opencode mcp list` и debug-команды могут зависеть от версии OpenCode; при отличии версии от 1.18.31 результат нужно отметить отдельно.
- Запуск `/plan` создаёт файл и потому требует контролируемого тестового имени и последующей уборки только собственного артефакта.
- Runtime-проверка MCP не должна вызывать аппаратные, сетевые или NVS-изменяющие операции.
- Нельзя автоматически исправлять ошибки скриптом `/verify` или PowerShell-проверкой.
- Нельзя считать успешным только прохождение PowerShell-скрипта: нужны проверки resolved-конфигурации, permissions, команд, Git и границ прошивки.

## Проверка результата

- `scripts/verify-agent-structure.ps1` завершён с кодом 0.
- `opencode --version` и `opencode debug config` проходят без ошибок; версия зафиксирована в отчёте.
- `opencode debug agent planner` подтверждает project planner и ограничение edit scope.
- `opencode mcp list` подтверждает project/global scope без вывода secrets.
- `/verify` read-only и корректно сообщает об ошибках.
- `/plan` создаёт реальный plan-файл в `.opencode/plans/` и печатает точный `PLAN:`-путь.
- Git показывает только ожидаемые инфраструктурные изменения; secrets и runtime artifacts не tracked.
- Отсутствуют legacy paths и дублирующаяся project skill.
- `AGENTS.md`, `README.MD`, `.gitignore` и фактическое дерево не противоречат целевой структуре.
- `src/`, `data/`, `include/`, `lib/` и `platformio.ini` не содержат незапланированных изменений.

## Файлы, которые нельзя изменять

В рамках этой проверки запрещено изменять:

- `src/**`;
- `data/**`;
- `include/**`;
- `lib/**`;
- `platformio.ini`;
- `secrets.h`, `src/secrets.h` и `.mcp/meteostation/.env`;
- `C:\Users\evgen\.config\opencode\opencode.jsonc` и любые другие глобальные OpenCode-файлы;
- `opencode.json`, `.gitignore`, `AGENTS.md`, `README.MD`, `.mcp/**`, `.opencode/agents/**`, `.opencode/commands/**`, `.opencode/skills/**`, `scripts/verify-agent-structure.ps1` и Git-файлы.

Единственное допустимое изменение для контролируемого теста — создание и удаление собственного временного файла в `.opencode/plans/`, если это необходимо для проверки `/plan`; постоянные изменения в рамках данного плана не выполняются.
