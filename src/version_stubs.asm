option casemap:none
EXTERN g_proxyExports:QWORD

.code
ProxyGetFileVersionInfoA PROC
    jmp QWORD PTR [g_proxyExports + 0*8]
ProxyGetFileVersionInfoA ENDP
ProxyGetFileVersionInfoByHandle PROC
    jmp QWORD PTR [g_proxyExports + 1*8]
ProxyGetFileVersionInfoByHandle ENDP
ProxyGetFileVersionInfoExA PROC
    jmp QWORD PTR [g_proxyExports + 2*8]
ProxyGetFileVersionInfoExA ENDP
ProxyGetFileVersionInfoExW PROC
    jmp QWORD PTR [g_proxyExports + 3*8]
ProxyGetFileVersionInfoExW ENDP
ProxyGetFileVersionInfoSizeA PROC
    jmp QWORD PTR [g_proxyExports + 4*8]
ProxyGetFileVersionInfoSizeA ENDP
ProxyGetFileVersionInfoSizeExA PROC
    jmp QWORD PTR [g_proxyExports + 5*8]
ProxyGetFileVersionInfoSizeExA ENDP
ProxyGetFileVersionInfoSizeExW PROC
    jmp QWORD PTR [g_proxyExports + 6*8]
ProxyGetFileVersionInfoSizeExW ENDP
ProxyGetFileVersionInfoSizeW PROC
    jmp QWORD PTR [g_proxyExports + 7*8]
ProxyGetFileVersionInfoSizeW ENDP
ProxyGetFileVersionInfoW PROC
    jmp QWORD PTR [g_proxyExports + 8*8]
ProxyGetFileVersionInfoW ENDP
ProxyVerFindFileA PROC
    jmp QWORD PTR [g_proxyExports + 9*8]
ProxyVerFindFileA ENDP
ProxyVerFindFileW PROC
    jmp QWORD PTR [g_proxyExports + 10*8]
ProxyVerFindFileW ENDP
ProxyVerInstallFileA PROC
    jmp QWORD PTR [g_proxyExports + 11*8]
ProxyVerInstallFileA ENDP
ProxyVerInstallFileW PROC
    jmp QWORD PTR [g_proxyExports + 12*8]
ProxyVerInstallFileW ENDP
ProxyVerLanguageNameA PROC
    jmp QWORD PTR [g_proxyExports + 13*8]
ProxyVerLanguageNameA ENDP
ProxyVerLanguageNameW PROC
    jmp QWORD PTR [g_proxyExports + 14*8]
ProxyVerLanguageNameW ENDP
ProxyVerQueryValueA PROC
    jmp QWORD PTR [g_proxyExports + 15*8]
ProxyVerQueryValueA ENDP
ProxyVerQueryValueW PROC
    jmp QWORD PTR [g_proxyExports + 16*8]
ProxyVerQueryValueW ENDP
END
