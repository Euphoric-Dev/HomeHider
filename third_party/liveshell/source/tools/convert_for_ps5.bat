@echo off
rem LiveShell Engine video converter. Drag one or more videos onto this file.
rem Any format in (mkv, mov, avi, webm, mp4, 10-bit, HDR, 120 fps ...) -> <name>_ps5.mp4 next to the original:
rem H.264 8-bit, max 60 fps, the size by the video (fitted with black bars):
rem   up to 1080p -> 1920x1080 / up to 1440p -> 2560x1440 (2K) / bigger -> 3840x2160 (4K, bigger is scaled down).
rem The H.264 level follows the size (1080p: 4.2 at most, 2K / 4K: 5.1 / 5.2; 4 reference frames keep 1080p at 4.2).
rem The sound is kept as it is (AAC stereo, no loudness change). The index goes first: quick start on the PS5.
rem Needs ffmpeg.exe in this folder (included in the release) or on PATH.
setlocal
set FFMPEG=%~dp0ffmpeg.exe
if not exist "%FFMPEG%" set FFMPEG=ffmpeg
"%FFMPEG%" -version >nul 2>&1
if errorlevel 1 (
  echo ffmpeg.exe was not found. Keep it in the same folder as this file.
  pause
  exit /b 1
)
if "%~1"=="" (
  echo Drag one or more video files onto convert_for_ps5.bat.
  pause
  exit /b 1
)

rem The box: by the pixel count - more than 2K -> 4K, more than 1080p -> 2K, else 1080p (a vertical 1080p clip
rem stays 1080p). The black bars: the smallest of those sizes the scaled picture fits in.
set SW=if(gt(iw*ih,3686400),3840,if(gt(iw*ih,2073600),2560,1920))
set SH=if(gt(iw*ih,3686400),2160,if(gt(iw*ih,2073600),1440,1080))
set PW=if(gt(iw,2560)+gt(ih,1440),3840,if(gt(iw,1920)+gt(ih,1080),2560,1920))
set PH=if(gt(iw,2560)+gt(ih,1440),2160,if(gt(iw,1920)+gt(ih,1080),1440,1080))

:next
if "%~1"=="" goto done
echo.
echo === %~nx1
"%FFMPEG%" -hide_banner -loglevel error -stats -y -i "%~1" ^
  -vf "scale=w='%SW%':h='%SH%':force_original_aspect_ratio=decrease:force_divisible_by=2:flags=lanczos,pad=w='%PW%':h='%PH%':x=(ow-iw)/2:y=(oh-ih)/2,format=yuv420p" ^
  -fpsmax 60 -c:v libx264 -preset slow -profile:v high -crf 17 -maxrate 40M -bufsize 60M ^
  -x264-params "keyint=60:min-keyint=30:scenecut=0:open-gop=0:ref=4" ^
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv ^
  -c:a aac -b:a 192k -ar 48000 -ac 2 ^
  -movflags +faststart "%~dpn1_ps5.mp4"
if errorlevel 1 (echo FAILED: %~nx1) else (echo OK: %~n1_ps5.mp4)
shift
goto next

:done
echo.
echo Done. Copy the _ps5.mp4 files to /data/LiveShell/video on the PS5, to a "liveshell"
echo folder on a USB drive, or upload them on the LiveShell Engine phone / PC page.
echo 4K and 2K videos play as the main wallpaper (or in PS Store only mode); for a game, use a 1080p one.
pause
