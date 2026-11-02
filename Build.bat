rc /nologo /fo app.res app.rc
cl.exe /EHsc /O2 /DUNICODE /D_UNICODE main.cpp app.res user32.lib gdi32.lib shell32.lib comctl32.lib dwmapi.lib dxva2.lib advapi32.lib /link /SUBSYSTEM:WINDOWS
