@echo off
rem deploy.bat - copy the built Gateway and Receiver (and the Pi scripts) to the Pi in one go.
rem Edit the next two lines for your laptop and your Pi.
set PI=10.106.201.14
set WS=C:\Users\SURESH\ide-8.0.3-workspace

rem On the Pi first (as root):  sh pi_run.sh stop     (a running program cannot be overwritten)
set OPT=-O -o Ciphers=aes256-ctr -o MACs=hmac-sha2-256
cd /d "%~dp0pi"
scp %OPT% "%WS%\Gateway\build\aarch64le-debug\Gateway" "%WS%\Reciever\build\aarch64le-debug\Reciever" "%WS%\Chassis\build\aarch64le-debug\Chassis" "%WS%\Powertrain\build\aarch64le-debug\Powertrain" "%WS%\Bcm\build\aarch64le-debug\Bcm" pi_run.sh start_gateway.sh gateway.conf qnxuser@%PI%:
echo.
echo Done. On the Pi (as root):  sh pi_run.sh start
pause
