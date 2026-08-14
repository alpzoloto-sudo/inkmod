@echo off
setlocal EnableExtensions
cd /d "%~dp0"

echo ==========================================
echo inkMOD FINAL REPOSITORY CLEANUP
echo ==========================================
echo.

if not exist ".git" (
  echo ERROR: Run this BAT from the root of the inkmod Git repository.
  pause
  exit /b 1
)

if not exist "platformio.ini" (
  echo ERROR: platformio.ini not found. Wrong folder.
  pause
  exit /b 1
)

echo [1/7] Checking protected files...
git status --porcelain -- README.md assets/inkmod-logo-light.png assets/inkmod-logo-dark.png web/assets/logo.png > "%TEMP%\inkmod_protected_status.txt"
for %%A in ("%TEMP%\inkmod_protected_status.txt") do if %%~zA GTR 0 (
  echo.
  echo WARNING: One or more protected files already have local changes:
  type "%TEMP%\inkmod_protected_status.txt"
  echo.
  echo This cleanup will NOT modify those files.
)
del /q "%TEMP%\inkmod_protected_status.txt" 2>nul

echo [2/7] Creating safety branch...
git show-ref --verify --quiet refs/heads/backup-final-cleanup
if errorlevel 1 (
  git branch backup-final-cleanup
  if errorlevel 1 (
    echo ERROR: Could not create backup branch.
    pause
    exit /b 1
  )
) else (
  echo backup-final-cleanup already exists - keeping it.
)

echo [3/7] Removing obsolete root documentation...
del /q "INKMOD_CUSTOM_FEATURES.md" 2>nul
del /q "RELEASE_CHECKLIST.md" 2>nul
del /q "RELEASE_NOTES_v1.1.4.md" 2>nul

echo [4/7] Removing obsolete development / migration documentation...
del /q "docs\COURSEWORK_SUBMISSION.md" 2>nul
del /q "docs\ARCHITECTURE_AUDIT.md" 2>nul
del /q "docs\CROSSPOINT_REMOVAL.md" 2>nul
del /q "docs\MIGRATION_PLAN.md" 2>nul
del /q "docs\READER_BASELINE.md" 2>nul
del /q "docs\catalog" 2>nul

echo [5/7] Removing old AI/editor leftovers if present...
rmdir /s /q ".agents" 2>nul
rmdir /s /q ".claude" 2>nul
rmdir /s /q ".dummy" 2>nul
rmdir /s /q ".vscode" 2>nul
del /q "AGENTS.md" 2>nul
del /q "CLAUDE.md" 2>nul
del /q "GOVERNANCE.md" 2>nul
del /q "SCOPE.md" 2>nul

echo [6/7] Removing unused duplicate tests/corpus placeholder...
del /q "tests\corpus\README.md" 2>nul
rmdir "tests\corpus" 2>nul
rmdir "tests" 2>nul

echo.
echo Protected files are intentionally untouched:
echo   README.md
echo   assets\inkmod-logo-light.png
echo   assets\inkmod-logo-dark.png
echo   web\assets\logo.png
echo.
echo Kept intentionally:
echo   .github, assets, bin, docs, freeink-sdk, include, lib
echo   managed_components, scripts, src, test, web
echo   CHANGELOG.md, USER_GUIDE.md, THIRD_PARTY.md
echo   build/configuration files
echo.

echo [7/7] Git status after cleanup:
echo ------------------------------------------
git status --short
echo ------------------------------------------
echo.

set /p CONFIRM=Commit and push this cleanup to origin/main now? [Y/N]: 
if /I not "%CONFIRM%"=="Y" (
  echo.
  echo Cleanup finished locally. Nothing was committed or pushed.
  pause
  exit /b 0
)

git add -A
if errorlevel 1 goto :fail

echo.
git diff --cached --name-status
echo.

set /p CONFIRM2=Final confirmation - commit and push the files shown above? [Y/N]: 
if /I not "%CONFIRM2%"=="Y" (
  echo.
  echo Files are staged, but nothing was committed or pushed.
  echo To undo staging: git restore --staged .
  pause
  exit /b 0
)

git commit -m "Final repository cleanup"
if errorlevel 1 (
  echo.
  echo Nothing to commit, or commit failed.
  pause
  exit /b 1
)

git push origin main
if errorlevel 1 goto :fail

echo.
echo ==========================================
echo DONE. Repository cleanup pushed to main.
echo README and logo files were not touched.
echo ==========================================
pause
exit /b 0

:fail
echo.
echo ERROR: A Git command failed.
echo Your files are still safe. Check git status.
pause
exit /b 1
