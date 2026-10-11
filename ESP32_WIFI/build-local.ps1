param(
    [string]$IdfPath = $env:IDF_PATH,
    [string]$ToolsPath = $env:IDF_TOOLS_PATH,
    [ValidateSet('build', 'size')]
    [string]$Action = 'build'
)
$ErrorActionPreference = 'Stop'
# Local fallback uses the existing installation; no downloads or installation.
if (!$IdfPath) {
    $IdfPath = 'G:\Application_deployment\esp32_idf_port\.build_tools\esp-idf'
}
if (!$ToolsPath) {
    $ToolsPath = 'G:\Application_deployment\esp32_idf_port\.build_tools\idf-tools'
}
$env:IDF_PATH = $IdfPath
$env:IDF_TOOLS_PATH = $ToolsPath
$env:IDF_PYTHON_ENV_PATH = Join-Path $ToolsPath 'python_env\idf5.4_py3.13_env'
$env:PYTHONDONTWRITEBYTECODE = '1'
$env:IDF_CCACHE_ENABLE = '0'
$env:IDF_COMPONENT_MANAGER = '0'
$env:ESP_ROM_ELF_DIR = Join-Path $ToolsPath 'tools\esp-rom-elfs\20241011'
$env:GIT_CONFIG_COUNT = '1'
$env:GIT_CONFIG_KEY_0 = 'safe.directory'
$env:GIT_CONFIG_VALUE_0 = $IdfPath
$toolDirs = @(
    (Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts'),
    (Join-Path $ToolsPath 'tools\cmake\3.30.2\bin'),
    (Join-Path $ToolsPath 'tools\ninja\1.12.1'),
    (Join-Path $ToolsPath 'tools\xtensa-esp-elf-gdb\14.2_20240403\xtensa-esp-elf-gdb\bin'),
    (Join-Path $ToolsPath 'tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin'),
    (Join-Path $ToolsPath 'tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin')
)
$env:Path = ($toolDirs -join ';') + ';' + $env:Path
$pythonExecutable = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
$idfScript = Join-Path $IdfPath 'tools\idf.py'
$buildDirectory = Join-Path $PSScriptRoot 'build-idf54'
& $pythonExecutable $idfScript -C $PSScriptRoot -B $buildDirectory $Action
exit $LASTEXITCODE
