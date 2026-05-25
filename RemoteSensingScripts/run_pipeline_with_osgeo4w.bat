@echo off
echo Initializing OSGeo4W Environment...
call C:\OSGeo4W\bin\o4w_env.bat
set PATH=%PATH%;C:\Program Files\ImageMagick-7.1.2-Q16-HDRI

echo.
echo Running DEM Pipeline...
echo Script: %~dp0process_dem_pipeline.py
echo.

python "%~dp0process_dem_pipeline.py"

echo.
if %errorlevel% neq 0 (
    echo Pipeline FAILED with error code %errorlevel%.
) else (
    echo Pipeline FINISHED successfully.
)
pause
