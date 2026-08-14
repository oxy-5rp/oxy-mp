# Проверочный сертификат для подписи oxyMP.
#
# Им проверяют, что вся цепочка подписи работает — сборка находит signtool,
# подпись ложится, файл после неё запускается, — ещё до того, как куплен
# настоящий сертификат.
#
# Чего он НЕ делает: не убирает предупреждение антивируса у друга. Самоподписанный
# сертификат доверен только на той машине, где его в это доверие внесли. На чужой
# машине он ничего не значит — там нужен настоящий сертификат от удостоверяющего
# центра (лучше EV, у него репутация SmartScreen с первого запуска). Этот же
# годится ровно на то, чтобы убедиться, что механизм подписи собран верно.
#
# Запуск (обычный PowerShell, права администратора не нужны):
#   powershell -ExecutionPolicy Bypass -File tools\signing\make-test-cert.ps1
#
# Сценарий печатает отпечаток. Дальше:
#   cmake --preset release -DOXYMP_SIGN=ON -DOXYMP_SIGN_THUMBPRINT=<отпечаток>
#   cmake --build --preset release

$ErrorActionPreference = 'Stop'

$subject = 'CN=oxyMP Test Signing'

# Прежний такой же сертификат убирается, чтобы их не копилось по одному на каждый
# запуск: они одноимённы, и signtool по имени уже не различил бы, которым из них
# подписывать.
Get-ChildItem Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq $subject } |
    ForEach-Object {
        Write-Host "Убираю прежний проверочный сертификат $($_.Thumbprint)"
        Remove-Item "Cert:\CurrentUser\My\$($_.Thumbprint)" -Force
    }

$cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject $subject `
    -CertStoreLocation Cert:\CurrentUser\My `
    -KeyUsage DigitalSignature `
    -KeyAlgorithm RSA `
    -KeyLength 2048 `
    -HashAlgorithm SHA256 `
    -NotAfter (Get-Date).AddYears(3)

# Чтобы подпись проверялась как доверенная НА ЭТОЙ машине, открытая часть
# сертификата вносится в доверенные корни и доверенные издатели текущего
# пользователя. Это местное доверие и дальше этой машины не идёт.
$publicCertPath = Join-Path $env:TEMP 'oxymp-test-cert.cer'
Export-Certificate -Cert $cert -FilePath $publicCertPath | Out-Null

try {
    Import-Certificate -FilePath $publicCertPath `
        -CertStoreLocation Cert:\CurrentUser\Root | Out-Null
    Import-Certificate -FilePath $publicCertPath `
        -CertStoreLocation Cert:\CurrentUser\TrustedPublisher | Out-Null
} finally {
    Remove-Item $publicCertPath -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host 'Проверочный сертификат готов и доверен на этой машине.'
Write-Host ''
Write-Host "Отпечаток: $($cert.Thumbprint)"
Write-Host ''
Write-Host 'Собирать с ним так:'
Write-Host "  cmake --preset release -DOXYMP_SIGN=ON -DOXYMP_SIGN_THUMBPRINT=$($cert.Thumbprint)"
Write-Host '  cmake --build --preset release'
Write-Host ''
Write-Host 'Проверить подпись собранного:'
Write-Host '  Get-AuthenticodeSignature build\msvc-x64\bin\Release\oxymp.exe'
