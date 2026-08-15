@echo off
rem Разовая подготовка машины к oxyMP: доверие издателю, исключение Защитника,
rem снятие метки «скачано из интернета».
rem
rem Всё делает setup.ps1 рядом; этот файл нужен ровно затем, чтобы попросить
rem права администратора: без них ни хранилище сертификатов, ни исключения
rem Защитника не тронуть.
rem
rem Батником, а не сценарием PowerShell напрямую: сценарии по умолчанию не
rem запускаются двойным щелчком вовсе, и человек упёрся бы в отказ, не поняв, что
rem он сделал не так.

powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','%~dp0setup.ps1'"
