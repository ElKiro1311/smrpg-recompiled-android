@echo off
rem Texturas HD con IA: amplía las texturas que el juego volcó (Menú > Vídeo > Imagen > «Volcar texturas») y las
rem deja en la carpeta texturas, que el juego usa con «Texturas HD». Necesita Real-ESRGAN ncnn Vulkan en la carpeta
rem realesrgan (junto a este archivo) o en la variable REALESRGAN, y un Python con Pillow.
rem Opciones: --escala 2 (por defecto), 3 o 4; --rehacer (vuelve a hacer las ya hechas).
chcp 65001 >nul
setlocal
set "AQUI=%~dp0"
if not defined REALESRGAN set "REALESRGAN=%AQUI%realesrgan\realesrgan-ncnn-vulkan.exe"
set PY=
for %%v in (3.12 3.13 3.11 3.14 3) do if not defined PY py -%%v -c "import PIL" >nul 2>&1 && set "PY=py -%%v"
if not defined PY python -c "import PIL" >nul 2>&1 && set "PY=python"
if not defined PY (
  echo No hay un Python con Pillow. Instálalo con:  py -m pip install pillow
  pause
  exit /b 1
)
%PY% "%AQUI%tools\texturas_hd.py" "%AQUI%." %*
pause
