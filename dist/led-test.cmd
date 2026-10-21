@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo.
echo  LED 校准：请先完全关闭游戏（chusanApp.exe），再运行本脚本。
echo  台子会依次显示 6 段颜色，每段 3 秒：
echo    A: 只把第 1 个字节设为 255
echo    B: 只把第 2 个字节设为 255
echo    C: 只把第 3 个字节设为 255
echo    D: 三个字节全 255
echo    E: 只有 LED 0 亮白
echo    F: 只有 LED 1 亮白
echo.
pause
probe.exe --led-test
echo.
type probe.log
echo.
echo  请把上面这段日志，以及每段实际看到的颜色发我。
pause
