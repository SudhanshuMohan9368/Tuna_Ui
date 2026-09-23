# Build / upload the TUNA UI sketch using only this repo's libraries/.
#   .\build.ps1                 compile
#   .\build.ps1 -Upload         compile + flash (default COM8)
#   .\build.ps1 -Upload -Port COM5 -Monitor
#   .\build.ps1 -Clean          full rebuild
#   .\build.ps1 -Setup          one-time: install the ESP32 core this project needs
param(
  [switch]$Upload,
  [string]$Port = "COM8",
  [switch]$Clean,
  [switch]$Monitor,
  [switch]$Setup
)
$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$cli = Join-Path $root "tools\arduino-cli.exe"
if (-not (Test-Path $cli)) { $cli = (Get-Command arduino-cli -ErrorAction Stop).Source }

# Sketchbook = this repo, so only libraries/ here is used (not Documents\Arduino\libraries).
$env:ARDUINO_DIRECTORIES_USER = $root

$coreVersion = "esp32:esp32@3.2.0"
$esp32Index = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"
$fqbn = "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=fatflash,PSRAM=opi,CDCOnBoot=cdc"

if ($Setup) {
  & $cli core update-index --additional-urls $esp32Index
  & $cli core install $coreVersion --additional-urls $esp32Index
  exit $LASTEXITCODE
}

$cliArgs = @("compile", "--fqbn", $fqbn, "--build-path", (Join-Path $root "build"))
if ($Clean)  { $cliArgs += "--clean" }
if ($Upload) { $cliArgs += @("--upload", "--port", $Port) }
$cliArgs += (Join-Path $root "tuna_ui")

& $cli @cliArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Monitor) { & $cli monitor --port $Port --config baudrate=115200 }
