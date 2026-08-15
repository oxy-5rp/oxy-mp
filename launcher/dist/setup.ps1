# Разовая подготовка машины к oxyMP.
#
# Запускается не сама, а из «Установить.cmd» — тот просит права администратора.
# Без них ничего из этого не сделать: и хранилище сертификатов, и исключения
# Защитника Windows принадлежат системе, а не пользователю.
#
# Делает ровно три вещи и ничего сверх этого.

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path

function Ok($text)   { Write-Host "  $text" -ForegroundColor Green }
function Fail($text) { Write-Host "  $text" -ForegroundColor Red }

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal $identity

if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail 'Нужны права администратора. Запустите «Установить.cmd», а не этот файл.'
    Read-Host 'Enter — закрыть'
    exit 1
}

Write-Host ''
Write-Host 'oxyMP — подготовка машины' -ForegroundColor Cyan
Write-Host ''

# 1. Издатель.
#
# oxyMP подписан собственным сертификатом. Windows такую подпись видит, но не
# знает, чей это издатель, — и потому встречает игрока окном «неизвестный
# издатель». Внеся сертификат в доверенные, мы отвечаем на этот вопрос один раз
# и навсегда.
#
# Настоящий, покупной сертификат этого шага не требовал бы вовсе: за него уже
# поручился удостоверяющий центр, которому Windows верит с рождения.
Write-Host 'Издатель oxyMP:'

$certificate = Join-Path $root 'oxymp.cer'

if (-not (Test-Path $certificate)) {
    Fail "нет файла $certificate — подготовка не нужна, сборка не подписана"
} else {
    Import-Certificate -FilePath $certificate -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $certificate -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Ok 'внесён в доверенные'
}

# 2. Защитник Windows.
#
# oxyMP внедряет свой модуль в процесс игры — иначе мультиплеера не построить, и
# ровно так же устроены RAGE MP и alt:V. Машинные проверки Защитника считают
# внедрение кода признаком вредоноса и, не зная нашей программы, уносят её в
# карантин прямо во время запуска.
#
# Исключение говорит Защитнику: этой папке я доверяю. Оно распространяется
# только на неё — на остальную систему проверки продолжают работать.
Write-Host 'Защитник Windows:'

try {
    Add-MpPreference -ExclusionPath $root
    Ok "папка исключена из проверок: $root"
} catch {
    Fail "не вышло добавить исключение: $($_.Exception.Message)"
    Fail 'Если Защитник заменён другим антивирусом, добавьте исключение в нём.'
}

# 3. Метка «скачано из интернета».
#
# Windows ставит её каждому файлу из архива и потом переспрашивает о каждом
# запуске. Снимаем — иначе SmartScreen встретит игрока лишним окном.
Write-Host 'Метка «скачано из интернета»:'

Get-ChildItem -Path $root -Recurse -File | Unblock-File
Ok 'снята'

Write-Host ''
Write-Host 'Готово. Запускайте oxymp.exe.' -ForegroundColor Cyan
Write-Host ''

Read-Host 'Enter — закрыть'
