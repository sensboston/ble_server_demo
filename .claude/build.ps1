Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:MINGW_PREFIX -ErrorAction SilentlyContinue

$env:IDF_PATH = 'D:\Espressif\frameworks\esp-idf-v5.5.3'
$env:IDF_TOOLS_PATH = 'D:\Espressif'
$env:IDF_PYTHON_ENV_PATH = 'D:\Espressif\python_env\idf5.5_py3.11_env'
$env:PATH = 'D:\Espressif\python_env\idf5.5_py3.11_env\Scripts;D:\Espressif\tools\cmake\3.30.2\bin;D:\Espressif\tools\ninja\1.12.1;D:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin;D:\Espressif\tools\idf-git\2.44.0\cmd;D:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\bin;C:\Windows\System32'

Set-Location 'D:\Projects\ble_server_demo'
& 'D:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' `
  'D:\Espressif\frameworks\esp-idf-v5.5.3\tools\idf.py' `
  $args
