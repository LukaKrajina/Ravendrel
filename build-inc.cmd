@echo off
rem 增量构建 rvdl_rt（需要 vcvars64 提供标准库的 LIB 搜索路径）
rem 用法: build-inc.cmd [target...]
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo VCVARS_FAILED
  exit /b 1
)
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd /d D:\Data\Project\Ravendrel
ninja -C runtime\build %*