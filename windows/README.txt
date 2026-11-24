Narval for Windows
==================

A text editor configured in Lisp. narval.exe runs in Windows Terminal (or any
Windows 10 1809+ / Windows 11 console); narval-gui.exe is the same editor in
a window of its own.

Install
-------

1. Unzip this folder anywhere.
2. In the folder, right-click install.ps1 > "Run with PowerShell".
   Or, from PowerShell in the folder:

       powershell -ExecutionPolicy Bypass -File .\install.ps1

   It copies narval.exe to %LOCALAPPDATA%\Programs\narval, adds that folder
   to your PATH, and sets up your configuration in %USERPROFILE%\.narval.
   narval-gui.exe and its DLLs (SDL3.dll, SDL3_ttf.dll) go in the same
   folder, and "Narval" is added to the Start menu (add -NoGui to skip it).
3. Open a new terminal and run:  narval myfile.txt
   Or search for Narval in the Start menu for the window app.

Re-running install.ps1 (from a newer release, say) updates narval and the
bundled packages; your configuration is never overwritten without asking.

To uninstall, run from PowerShell in the folder:

    powershell -ExecutionPolicy Bypass -File .\install.ps1 -Uninstall

It removes narval.exe, narval-gui.exe, the Start menu entry and the PATH
entry, and keeps %USERPROFILE%\.narval; add -Purge to delete that too.

If Windows warns about the files
--------------------------------

narval.exe and narval-gui.exe aren't code-signed, so SmartScreen may say "Windows protected your
PC": click "More info", then "Run anyway". If PowerShell refuses to run
install.ps1, use the -ExecutionPolicy Bypass command above, or first run:

    Get-ChildItem -Recurse | Unblock-File

Recommended
-----------

Git for Windows (winget install Git.Git): git status in the status line,
the git package (CTRL-x g), and project search inside repositories use it.

Your own settings go in %USERPROFILE%\.narval\packages\personal-config\init.lisp
(never replaced by updates).

Keys: CTRL-x CTRL-s save, CTRL-x CTRL-c quit, CTRL-x f open a file.
