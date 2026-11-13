@echo off
rem run-sim.cmd -- start the simulator from Windows (through WSL): double-click it, or run it from
rem PowerShell / cmd. It runs this clone's build/sim/rechord_sim_8g on the image in RECHORD_IMG
rem (default ~/rechord/internal.img, on Ubuntu's own disk: much faster than an image under /mnt/c).
rem
rem   sim\run-sim.cmd                     the 8 GB board  (build it: make sim BOARD=8g)
rem   sim\run-sim.cmd --512               the 512 MB board (build it: make sim)
rem   sim\run-sim.cmd --scale 4 --gain 30 any rechord_sim option passes through
rem   set RECHORD_IMG=~/other.img         another image (a WSL path)
setlocal
rem The clone's root, taken before any shift (shift moves %0 too).
set "ROOT=%~dp0.."
set "BIN=build/sim/rechord_sim_8g"
set "HINT=make sim BOARD=8g"
if /i "%~1"=="--512" (
    set "BIN=build/sim/rechord_sim"
    set "HINT=make sim"
    shift
)
if not defined RECHORD_IMG set "RECHORD_IMG=~/rechord/internal.img"
set "ARGS="
:collect
if "%~1"=="" goto run
set "ARGS=%ARGS% %1"
shift
goto collect
:run
wsl --cd "%ROOT%" -e bash -c "[ -x %BIN% ] || { echo '%BIN% not built: %HINT%'; exit 1; }; [ -f %RECHORD_IMG% ] || { echo '%RECHORD_IMG%: no image (sim/mkimage.sh MUSIC_DIR %RECHORD_IMG%)'; exit 1; }; exec %BIN%%ARGS% %RECHORD_IMG%"
if errorlevel 1 pause
