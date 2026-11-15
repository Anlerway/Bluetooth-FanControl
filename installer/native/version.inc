// 版本资源块：setup.rc / uninstall.rc 共用，版本号由 build.bat 写入 build\version.h。
#include <winver.h>

#ifndef FC_VER_MAJOR
#define FC_VER_MAJOR 2
#endif
#ifndef FC_VER_MINOR
#define FC_VER_MINOR 0
#endif
#ifndef FC_VER_PATCH
#define FC_VER_PATCH 0
#endif
#ifndef FC_VER_STRING
#define FC_VER_STRING "2.0.0"
#endif
#ifndef FC_FILENAME
#define FC_FILENAME "setup.exe"
#endif
#ifndef FC_DESCRIPTION
#define FC_DESCRIPTION "FanControl Setup"
#endif

1 VERSIONINFO
 FILEVERSION FC_VER_MAJOR,FC_VER_MINOR,FC_VER_PATCH,0
 PRODUCTVERSION FC_VER_MAJOR,FC_VER_MINOR,FC_VER_PATCH,0
 FILEFLAGSMASK 0x3fL
 FILEFLAGS 0x0L
 FILEOS 0x40004L
 FILETYPE 0x1L
 FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "080404b0"
        BEGIN
            VALUE "CompanyName", "Anlerway\0"
            VALUE "FileDescription", FC_DESCRIPTION
            VALUE "FileVersion", FC_VER_STRING "\0"
            VALUE "InternalName", "FanControl\0"
            VALUE "LegalCopyright", "Copyright (C) 2026 Anlerway\0"
            VALUE "OriginalFilename", FC_FILENAME
            VALUE "ProductName", "FanControl\0"
            VALUE "ProductVersion", FC_VER_STRING "\0"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x804, 1200
    END
END
