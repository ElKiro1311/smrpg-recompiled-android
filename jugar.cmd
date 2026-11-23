@echo off
rem SMRPG Recomp: arranca el juego con sonido, mando y teclado.
rem Necesita el juego extraído en extracted\ y el ejecutable compilado en build\ (MSYS2 UCRT64).
rem Controles (estilo Switch por la posición del botón):
rem   moverse: stick izquierdo / cruceta; teclado W A S D o flechas
rem   A (hablar, aceptar, atacar): botón derecho del mando (B de Xbox) / K
rem   B (saltar, cancelar): botón inferior (A de Xbox) / J
rem   X (objetos): botón superior (Y de Xbox) / L      Y (especial): botón izquierdo (X de Xbox) / I
rem   + (menú): Start / Enter       - : Back / Retroceso      L / R: Q / E      ZL / ZR: 1 / 3
rem   Esc o F1: menú del runtime (pestaña de controles para cambiarlos); F11 o Alt+Enter: pantalla completa
cd /d "%~dp0"
if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
set "PATH=%MSYS2_ROOT%\ucrt64\bin;%PATH%"
set LARECOMP_INPUT=
set LARECOMP_NO_AUDIO=
build\smrpg-recomp.exe --play extracted
