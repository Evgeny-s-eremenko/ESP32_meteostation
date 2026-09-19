$ErrorActionPreference = "Stop"

$root = (Get-Location).Path
$failures = [System.Collections.Generic.List[string]]::new()

function Require-Path([string] $relativePath) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $relativePath))) {
        $failures.Add("Missing: $relativePath")
    }
}

function Forbid-Path([string] $relativePath) {
    if (Test-Path -LiteralPath (Join-Path $root $relativePath)) {
        $failures.Add("Forbidden legacy path: $relativePath")
    }
}

Require-Path ".opencode/agents/esp32.md"
Require-Path ".opencode/agents/planner.md"
Require-Path ".opencode/commands/plan.md"
Require-Path ".opencode/commands/verify.md"
Require-Path ".opencode/plans"
Require-Path ".opencode/skills/embedded-systems/SKILL.md"
Require-Path ".mcp/meteostation/server.py"
Require-Path ".mcp/meteostation/requirements.txt"
Require-Path "opencode.json"
Require-Path "AGENTS.md"
Require-Path "skills-lock.json"

Forbid-Path ".agents/skills/embedded-systems"
Forbid-Path "mcp_server/server.py"
Forbid-Path ".playwright-mcp"

$localEnv = Join-Path $root ".mcp/meteostation/.env"
$localEnvExample = Join-Path $root ".mcp/meteostation/.env.example"
if (-not (Test-Path -LiteralPath $localEnv) -and -not (Test-Path -LiteralPath $localEnvExample)) {
    $failures.Add("Missing MCP secrets template: .mcp/meteostation/.env.example")
}

$skill = Get-Content -Raw -LiteralPath (Join-Path $root ".opencode/skills/embedded-systems/SKILL.md")
if ($skill -notmatch '(?m)^name:\s*embedded-systems\s*$') {
    $failures.Add("Skill frontmatter has no matching name")
}
if ($skill -notmatch '(?m)^description:\s*\S') {
    $failures.Add("Skill frontmatter has no description")
}

$projectConfig = Get-Content -Raw -LiteralPath (Join-Path $root "opencode.json")
if ($projectConfig -notmatch '"meteostation"') {
    $failures.Add("Project config does not register meteostation")
}

$globalConfigPath = Join-Path $env:USERPROFILE ".config/opencode/opencode.jsonc"
$globalConfig = Get-Content -Raw -LiteralPath $globalConfigPath
if ($globalConfig -match '"meteostation"') {
    $failures.Add("Global config still registers meteostation")
}
if ($globalConfig -notmatch '"playwright"') {
    $failures.Add("Global config does not register playwright")
}
if ($globalConfig -notmatch '(?s)"playwright".*?"enabled"\s*:\s*false') {
    $failures.Add("Global playwright MCP is not disabled by default")
}

$tracked = git ls-files
$secretPatterns = @(
    'METEOSTATION_PASSWORD\s*[:=]\s*(?!your_password_here\b|secret\b|\{env:)[^\s]+',
    'Authorization\s*[:=]\s*Bearer\s+(?!\{env:)',
    'SECRET_.*PASSWORD\s*[:=]\s*(?!your_|secret\b|\{env:)[^\s]+'
)
foreach ($file in $tracked) {
    if ($file -match '(^|[\\/])\.env$|secrets\.h$') {
        $failures.Add("Secret-like tracked file: $file")
        continue
    }
    if ($file -match '(^|[\\/])\.env\.example$') {
        continue
    }
    $fullPath = Join-Path $root $file
    if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
        $text = Get-Content -Raw -LiteralPath $fullPath
        foreach ($pattern in $secretPatterns) {
            if ($text -match $pattern) {
                $failures.Add("Possible secret in tracked file: $file")
                break
            }
        }
    }
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Output "Agent infrastructure checks passed."
