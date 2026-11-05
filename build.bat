@echo off
setlocal

rem ============================================================================
rem  Windows build script (rvdl_rt)
rem ----------------------------------------------------------------------------
rem  Prerequisites (edit paths to match your machine):
rem    - Visual Studio (vcvars64.bat + MSVC cl.exe)
rem    - LLVM (LLVMConfig.cmake)
rem    - Kokkos (KokkosConfig.cmake)
rem ============================================================================

rem cd to script dir (project root)
cd /d "%~dp0"

rem ---------------------------------------------------------------------------
rem  Dependency paths (edit as needed)
rem ---------------------------------------------------------------------------
set "VCVARS64=C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CLANG_CL=C:\Program Files\LLVM\bin\clang-cl.exe"
set "LLVM_DIR=C:\Libraries\LLVM-md\lib\cmake\llvm"
set "KOKKOS_DIR=C:\Libraries\kokkos\lib\cmake\Kokkos"
set "CUDA_ROOT=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.4"
set "ZLIB_ROOT=C:\Libraries\zlib"
set "ZSTD_ROOT=C:\Libraries\zstd"

rem ---------------------------------------------------------------------------
rem  Initialize vcvars64
rem ---------------------------------------------------------------------------
if not exist "%VCVARS64%" (
    echo [build.bat] vcvars64.bat not found: %VCVARS64%
    echo [build.bat] Please edit VCVARS64 in build.bat and retry.
    exit /b 1
)
call "%VCVARS64%"
if errorlevel 1 ( echo VCVARS_FAILED & exit /b 1 )

rem ---------------------------------------------------------------------------
rem  CMake configure (use clang-cl)
rem ---------------------------------------------------------------------------
rem  RC 编译器必须显式指定为 llvm-rc：clang-cl 工具链下 CMake 会生成
rem  cmake_llvm_rc 包装规则（clang 预处理 + RC 编译），若该规则配上自动探测到的
rem  Windows SDK rc.exe，会把 clang 风格参数喂给 rc.exe 导致资源编译失败。
if not exist runtime\build (
    echo === CONFIGURE ===
    "%CMAKE%" -S runtime -B runtime\build -G Ninja ^
      -DCMAKE_C_COMPILER="%CLANG_CL%" ^
      -DCMAKE_CXX_COMPILER="%CLANG_CL%" ^
      -DLLVM_DIR="%LLVM_DIR:\=/%" ^
      -DKokkos_DIR="%KOKKOS_DIR:\=/%" ^
      -DCUDAToolkit_ROOT="%CUDA_ROOT:\=/%" ^
      -DZLIB_ROOT="%ZLIB_ROOT:\=/%" ^
      -Dzstd_DIR="C:/Libraries/zstd/lib/cmake/zstd" ^
      -DCMAKE_PREFIX_PATH="C:/Libraries/zstd" ^
      -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_RC_COMPILER="C:/Program Files/LLVM/bin/llvm-rc.exe" ^
      -DOpenMP_CXX_FLAGS=-fopenmp ^
      -DOpenMP_CXX_LIB_NAMES=libomp ^
      -DOpenMP_libomp_LIBRARY=C:/Program Files/LLVM/lib/libomp.lib ^
      -DCMAKE_POLICY_DEFAULT_CMP0074=NEW ^
      -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
    if errorlevel 1 ( echo CONFIGURE_FAILED & exit /b 1 )
)

echo === BUILD rvdl_rt + runtime ===
"%CMAKE%" --build runtime\build --target rvdl_rt runtime -j 8
if errorlevel 1 ( echo BUILD_FAILED & exit /b 1 )

echo === DONE ===
endlocal