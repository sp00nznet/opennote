cmake_minimum_required(VERSION 3.16)
project(OpenNote C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin)

# SQLite library
add_library(sqlite3 STATIC
    lib/sqlite/sqlite3.c
)
target_include_directories(sqlite3 PUBLIC lib/sqlite)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_JSON1
    SQLITE_THREADSAFE=1
)

# Windows-specific SQLite settings
if(WIN32)
    target_compile_definitions(sqlite3 PRIVATE
        SQLITE_OS_WIN=1
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# Scintilla library
add_subdirectory(lib/scintilla)

# Main application sources
set(SUPERNOTE_SOURCES
    src/main.c
    src/app.c
    src/ui/mainwindow.c
    src/ui/tabcontrol.c
    src/ui/editor.c
    src/ui/editor_rich.c
    src/ui/toolbar.c
    src/ui/menubar.c
    src/ui/statusbar.c
    src/ui/dialogs.c
    src/core/document.c
    src/core/fileio.c
    src/core/search.c
    src/core/strbuf.c
    src/core/doctree.c
    src/core/doctree_rtf.c
    src/core/doctree_view.c
    src/core/imagedib.c
    src/layout/layout.cpp
    src/layout/layoutprint.cpp
    src/layout/layoutimage.cpp
    src/ui/pageview.cpp
    src/ui/richole.c
    src/core/docx.c
    src/pdf/pdfread.cpp
    src/pdf/pdfview.cpp
    src/db/database.c
    src/db/notes_repo.c
    src/db/links_repo.c
    src/sync/oauth.c
    src/sync/crypto.c
    src/sync/github_sync.c
    src/sync/google_sync.c
)

# Resource file
if(WIN32)
    set(SUPERNOTE_RESOURCES res/supernote.rc)
endif()

# Main executable
add_executable(OpenNote WIN32
    ${SUPERNOTE_SOURCES}
    ${SUPERNOTE_RESOURCES}
)

target_include_directories(OpenNote PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/lib/sqlite
    ${CMAKE_SOURCE_DIR}/lib/scintilla/scintilla/include
    ${CMAKE_SOURCE_DIR}/lib/scintilla/lexilla/include
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(OpenNote PRIVATE
    sqlite3
    scintilla
    comctl32
    comdlg32
    gdi32
    user32
    shell32
    shlwapi
    imm32
    ole32
    oleaut32
    uuid
    msimg32
    shcore
    winhttp
    ws2_32
    crypt32
    bcrypt
    xmllite
    dwrite
    d2d1
    d3d11
    dxgi
    windowscodecs
    runtimeobject
)

target_compile_definitions(OpenNote PRIVATE
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
)

# OAuth client IDs (optional).
#
# Client IDs only. A client *secret* is deliberately not accepted here: a secret
# compiled into a binary handed to users is not a secret, and OpenNote no longer
# needs one -- GitHub uses the device flow and Google uses PKCE with the user's
# own credentials. See SECURITY.md.
#
# Leaving both unset produces a build with no credentials in it at all; the user
# supplies a client ID in Settings if they want sync.
if(GH_OAUTH_CLIENT_ID)
    target_compile_definitions(OpenNote PRIVATE GITHUB_CLIENT_ID="${GH_OAUTH_CLIENT_ID}")
endif()
if(GOOGLE_CLIENT_ID)
    target_compile_definitions(OpenNote PRIVATE GOOGLE_CLIENT_ID="${GOOGLE_CLIENT_ID}")
endif()

if(GH_OAUTH_CLIENT_SECRET OR GOOGLE_CLIENT_SECRET)
    message(FATAL_ERROR
        "GH_OAUTH_CLIENT_SECRET and GOOGLE_CLIENT_SECRET are no longer accepted. "
        "A client secret shipped inside the binary is recoverable with `strings`. "
        "See SECURITY.md.")
endif()

# Enable warnings
if(MSVC)
    target_compile_options(OpenNote PRIVATE /W4)
    # Use external manifest
    set_target_properties(OpenNote PROPERTIES
        LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_SOURCE_DIR}/res/manifest.xml"
    )
else()
    target_compile_options(OpenNote PRIVATE -Wall -Wextra)
endif()
