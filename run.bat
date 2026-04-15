@echo off

mkdir build
copy ".\external\*.dll"  ".\build\"

:: set enviroment vars and requred stuff for the msvc compiler
call "D:\Programming\Software\msvc\setup_x64.bat"

set CFLAGS=/Zi /EHsc /D_AMD64_ /fp:fast /W4 /MD /nologo /utf-8 /std:c++17 /arch:AVX
:: /SUBSYSTEM:CONSOLE , /SUB-SYSTEM:WINDOWS 
set L_FLAGS=/SUBSYSTEM:CONSOLE
set SRC=..\main.cpp ..\external\imgui\imgui.cpp ..\external\imgui\backends\imgui_impl_glfw.cpp ..\external\imgui\backends\imgui_impl_opengl3.cpp ..\external\imgui\imgui_demo.cpp ..\external\imgui\imgui_draw.cpp ..\external\imgui\imgui_tables.cpp ..\external\imgui\imgui_widgets.cpp ..\external\implot\implot.cpp ..\external\implot\implot_demo.cpp ..\external\implot\implot_items.cpp ..\external\implot3d\implot3d.cpp ..\external\implot3d\implot3d_demo.cpp ..\external\implot3d\implot3d_items.cpp ..\external\implot3d\implot3d_meshes.cpp ..\external\ImAnim\im_anim.cpp ..\external\ImAnim\im_anim_demo.cpp ..\external\ImAnim\im_anim_usecase.cpp ..\external\ImAnim\im_anim_doc.cpp ..\external\ImGuiColorTextEdit\TextEditor.cpp ..\external\ImGuiColorTextEdit\TextDiff.cpp  ..\external\ImGuiColorTextEdit\example\dejavu.cpp ..\external\imgui-knobs\imgui-knobs.cpp ..\external\ImGuiFileDialog\ImGuiFileDialog.cpp ..\external\glad.c ..\external\cJSON.c
:: ..\external\ImGuiColorTextEdit\example\editor.cpp
set INCLUDE_DIRS=/I..\external\ /I..\external\imgui\ /I..\external\imgui\backends\ /I..\external\implot\ /I..\external\implot3d\ /I..\external\ImAnim\ /I..\external\ImGuiColorTextEdit\ /I..\external\ImGuiColorTextEdit\example\ /I..\external\ImGuiFileDialog\ /I..\external\imgui-knobs\
set LIBRARY_DIRS=/LIBPATH:..\external\
set LIBRARIES=opengl32.lib glfw3.lib nfd.lib sqlite3.lib glew32.lib UxTheme.lib Dwmapi.lib ole32.lib user32.lib gdi32.lib shell32.lib kernel32.lib ole32.lib mmdevapi.lib uuid.lib psapi.lib

if "%1"=="" (
    echo Usage: run.bat [rel|dbg]
    exit /b 1
)

if "%1"=="rel" (
    echo Building the project...
    pushd .\build
    cl %CFLAGS% /O2 %INCLUDE_DIRS% %SRC% /link %LIBRARY_DIRS% %LIBRARIES% %L_FLAGS%
    if errorlevel 1 (
        goto :build_failed
    )
    echo Build successful. Running...
    .\Main.exe
    goto :build_success
)

if "%1"=="dbg" (
    echo Building the project with debugging symbols...
    pushd .\build
    cl %CFLAGS% /fsanitize=address %INCLUDE_DIRS% %SRC% /link %LIBRARY_DIRS% %LIBRARIES% %L_FLAGS%
    if errorlevel 1 (
        goto :build_failed
    )
    echo Debug build successful. Launching debugger...
    start "" "devenv.exe" .\Main.exe
    goto :build_success
)

echo Unknown command: %1
exit /b 1

:build_failed
echo Build failed!
popd
exit /b 1

:build_success
popd
exit /b 0
