# Сборка Node.js для скриптового слоя oxyMP.
#
# Запускается руками и один раз на машину: Node собирается долго, и держать это
# внутри обычной сборки проекта нельзя — тогда всякий, кто впервые собирает
# oxyMP, ждал бы полчаса, не понимая, чего.
#
# Почему своей сборкой, а не готовой. У alt:V libnode берётся с их CDN; он же
# лежит в js-module-v1 на диске. Взять его нельзя: это сборка Release, а
# отладочной взять неоткуда — cdn.alt-mp.com недоступен. Отладочный сервер с
# выпускным Node не линкуется: у отладочной библиотеки времени выполнения другой
# _ITERATOR_DEBUG_LEVEL, и всякая стандартная строка в сигнатуре расходится.
#
# Что нужно на машине, кроме этого сценария:
#
#   Исходники Node. Выкачиваются один раз:
#     git clone --depth 1 --branch v24.19.0 https://github.com/nodejs/node.git D:\oxymp-deps\node
#
#   Visual Studio 2026 с «Разработка классических приложений на C++», а к ней —
#     «Компилятор C++ Clang для Windows» и «Поддержка MSBuild для набора
#     инструментов LLVM». Компилятор Clang здесь не прихоть: начиная с двадцать
#     четвёртой версии Node на Windows собирается только им, и его собственный
#     vcbuild.bat отказывается работать иначе.
#
#   NASM — для ассемблерных вставок OpenSSL. Ставить его в систему не нужно:
#     достаточно распакованного архива, путь к которому передаётся ключом.
#     https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/
#
#   Python. Его требует система сборки Node (GYP). Годится и 3.14.
#
# Полученное остаётся рядом с исходниками, и оттуда же его находит CMake:
#
#     cmake --preset msvc-x64 -DOXYMP_NODE_ROOT="D:\oxymp-deps\node"

[CmdletBinding()]
param(
    [string]$NodeRoot = "D:\oxymp-deps\node",

    # Обе нужны обе: отладочный сервер обязан линковаться с отладочным Node.
    [ValidateSet("Debug", "Release", "Both")]
    [string]$Configuration = "Both",

    # Каталог с nasm.exe. Пусто — надеемся, что он в PATH.
    [string]$NasmPath = "D:\oxymp-deps\nasm-2.16.03",

    # Где стоит Visual Studio. Пусто — найдём сами.
    [string]$VisualStudio = ""
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path "$NodeRoot\configure")) {
    throw "Исходников Node в '$NodeRoot' нет. Выкачайте их: git clone --depth 1 --branch v24.19.0 https://github.com/nodejs/node.git `"$NodeRoot`""
}

if (-not $VisualStudio) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "Не найден vswhere.exe — Visual Studio не установлена?"
    }
    $VisualStudio = & $vswhere -latest -property installationPath
}

$vcvars = "$VisualStudio\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    throw "Не найден '$vcvars'. Нужна Visual Studio с рабочей нагрузкой C++."
}

# Есть ли clang-cl. Проверяется до сборки: без него Node 24 не соберётся, и
# узнать об этом лучше сразу, а не через двадцать минут работы.
$clang = "$VisualStudio\VC\Tools\Llvm\x64\bin\clang-cl.exe"
if (-not (Test-Path $clang)) {
    throw @"
Не найден clang-cl: '$clang'.
Начиная с 24-й версии Node на Windows собирается только им. Добавьте в
Visual Studio компоненты «C++ Clang Compiler for Windows» и «MSBuild support
for LLVM (clang-cl) toolset».
"@
}

$clangVersion = (& $clang --version | Select-String -Pattern "clang version (\S+)").Matches[0].Groups[1].Value
Write-Output "clang-cl: $clangVersion"

# NASM добавляется только в окружение этого запуска. В PATH системы он не нужен:
# кроме сборки Node, его здесь никто не спрашивает.
if ($NasmPath -and (Test-Path "$NasmPath\nasm.exe")) {
    $env:PATH = "$NasmPath;$env:PATH"
    Write-Output "NASM: $NasmPath"
} elseif (-not (Get-Command nasm -ErrorAction SilentlyContinue)) {
    Write-Warning "NASM не найден — OpenSSL соберётся без ассемблерных вставок (медленнее)."
}

$configurations = if ($Configuration -eq "Both") { @("Release", "Debug") } else { @($Configuration) }

# Настройка и сборка ведутся напрямую, а не через vcbuild.bat, и это вынужденно.
#
# Ключ `release` у vcbuild включает LTCG, а тот собирает библиотеки через
# lld-link, и внутри них оказывается биткод LLVM. Линковщик MSVC читать его не
# умеет и отвечает невнятным «LNK1136: недопустимый или повреждённый файл».
# Выключателя у vcbuild для этого нет.
#
# GYP_MSVS_VERSION обязателен. Без него configure порождает решение формата
# 2005 года с файлами .vcproj, и msbuild отказывается его открывать словами
# «отсутствует корневой элемент».
$configureFlags = "--shared --dest-cpu=x64 --clang-cl=$clangVersion"

Write-Output ""
Write-Output "Настройка: configure $configureFlags"

$configure = "cd /d `"$NodeRoot`" && call `"$vcvars`" x64 && set GYP_MSVS_VERSION=2026&& python configure $configureFlags"
& cmd /c $configure

if ($LASTEXITCODE -ne 0) {
    throw "Настройка Node не удалась (код $LASTEXITCODE)."
}

# Правка порождённого описания сборки: библиотеке недостаёт winmm.
#
# Это пробел в самом Node, а не у нас. В его node.gyp winmm перечислен, но до
# цели `libnode` не доходит, и в режиме --shared она не собирается вовсе:
# «undefined symbol: timeGetTime». Собранный обычным способом Node этого не
# замечает — там winmm подключает исполняемый файл.
#
# Правится порождённый файл, а не исходник: настройка перезаписывает его при
# каждом запуске, и правка ложится заново вместе с ним. Исходники Node остаются
# нетронутыми, и обновить их можно простым git pull.
$project = Join-Path $NodeRoot "libnode.vcxproj"

if (-not (Test-Path $project)) {
    throw "После настройки не появился '$project'."
}

$text = [System.IO.File]::ReadAllText($project)

if ($text -notmatch "winmm\.lib") {
    $text = $text -replace "<AdditionalDependencies>", "<AdditionalDependencies>winmm.lib;"
    [System.IO.File]::WriteAllText($project, $text)
    Write-Output "libnode.vcxproj: добавлен winmm.lib"
}

foreach ($config in $configurations) {
    Write-Output ""
    Write-Output "=== Node $config ==="
    Write-Output "Это надолго: первая сборка занимает около получаса."

    $started = Get-Date

    # Цель libnode, а не всё подряд: node.exe, средства сборки снимков и набор
    # проверок нам не нужны, а времени берут больше самой библиотеки.
    $build = "cd /d `"$NodeRoot`" && call `"$vcvars`" x64 && msbuild node.sln /m /t:libnode /p:Configuration=$config /p:Platform=x64 /clp:NoItemAndPropertyList;Verbosity=minimal /nologo"
    & cmd /c $build

    if ($LASTEXITCODE -ne 0) {
        throw "Сборка Node $config провалилась (код $LASTEXITCODE). Смотрите вывод выше."
    }

    Write-Output "Node $config собран за $([int]((Get-Date) - $started).TotalMinutes) мин"
}

# Что получилось — перечислить явно. Дальше это ищет cmake/node.cmake, и если
# имена окажутся не те, узнать об этом лучше сейчас, а не при сборке сервера.
Write-Output ""
Write-Output "=== Что получилось ==="

foreach ($config in $configurations) {
    foreach ($file in @("out\$config\libnode.dll", "out\$config\lib\libnode.lib")) {
        $path = Join-Path $NodeRoot $file
        if (Test-Path $path) {
            Write-Output ("    {0,-40} {1,8:N1} МБ" -f $file, ((Get-Item $path).Length / 1MB))
        } else {
            Write-Warning "нет $file"
        }
    }
}

Write-Output ""
Write-Output "Дальше: передайте CMake путь к исходникам Node —"
Write-Output "    cmake --preset msvc-x64 -DOXYMP_NODE_ROOT=`"$NodeRoot`""
