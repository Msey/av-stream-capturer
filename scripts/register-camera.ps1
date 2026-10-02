$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$candidates = @(
  (Join-Path $root "build\Release\ConferenceFxCam.dll"),
  (Join-Path $root "build\Debug\ConferenceFxCam.dll")
)
$dll = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $dll) {
  Write-Error "Сначала соберите Release: ConferenceFxCam.dll не найден."
}
Write-Host "Регистрация $dll"
& regsvr32.exe /s $dll
if ($LASTEXITCODE -ne 0) {
  Write-Error "regsvr32 завершился с кодом $LASTEXITCODE. Запустите скрипт в обычном окне: регистрация идёт в HKCU и права администратора не нужны, если вызывается DllRegisterServer из самой программы."
}
Write-Host "Готово. В созвоне выберите камеру Conference FX Camera."
