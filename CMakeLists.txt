cmake_minimum_required(VERSION 3.21)

project(DIAssetBrowser
    VERSION 2.0.0
    DESCRIPTION "Native C++ Diablo Immortal asset browser - reads the PC client's MPK store directly"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sources are UTF-8 without BOM; without this MSVC reads them as the ANSI code
# page and every non-ASCII UI glyph ("Filters \u25be", chips, middots) mojibakes.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# lz4: vcpkg provides a CONFIG package (the normal path on Windows); the Linux
# verification sandbox has only the system lib — fall back to find_library there.
find_package(lz4 CONFIG QUIET)
if(NOT TARGET lz4::lz4)
    find_library(LZ4_LIB lz4 REQUIRED)
    find_path(LZ4_INC lz4.h REQUIRED)
    add_library(lz4::lz4 UNKNOWN IMPORTED)
    set_target_properties(lz4::lz4 PROPERTIES
        IMPORTED_LOCATION "${LZ4_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${LZ4_INC}")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Network)

qt_standard_project_setup()

# AUTORCC is NOT enabled by qt_standard_project_setup() — without it res/app.qrc
# is listed, configures without complaint, and is simply never compiled: the
# window icon silently stays the default (D4-project lesson).
set(CMAKE_AUTORCC ON)

# rc.exe reads app.ico THROUGH app.rc; replacing the .ico leaves the .rc
# byte-identical, so ninja would see nothing to do and keep the OLD icon.
#
# GUARDED on the file existing. There is no app.ico right now (the placeholder
# was removed until a real one is designed), and an OBJECT_DEPENDS on a missing
# file makes ninja fail the whole build with "missing and no known rule to make
# it" — the icon being absent must not stop the tool compiling.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico")
    set_property(SOURCE res/app.rc APPEND PROPERTY
                 OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico")
endif()

# ── storage layer (framework-free: std only) ────────────────────────────────
add_library(di_store STATIC
    src/store/Zzz4.h        src/store/Zzz4.cpp
    src/store/MpkIndex.h    src/store/MpkIndex.cpp
    src/store/Repository.h  src/store/Repository.cpp
    src/store/AssetStore.h  src/store/AssetStore.cpp)
target_include_directories(di_store PUBLIC src)
target_link_libraries(di_store PUBLIC lz4::lz4)

# ── console prover ──────────────────────────────────────────────────────────
# AnimParser.cpp is compiled in directly (it is std-only, no Qt) so the probe's
# --anim-verify mode runs the REAL parser over every clip — the verdict on a
# format fix comes from the code that ships, not a reimplementation that could
# drift. The rest of src/model stays out: it needs Qt.
add_executable(di_probe tools/di_probe.cpp src/model/AnimParser.cpp)
target_link_libraries(di_probe PRIVATE di_store)

# ── minidump carver (pulls the packed decoder out of a live-process .DMP) ────
# Framework-free, links nothing but the std lib.
add_executable(di_dumpcarve tools/di_dumpcarve.cpp)

# ── the app ─────────────────────────────────────────────────────────────────
qt_add_executable(DIAssetBrowser WIN32
    res/app.qrc     # window icon, embedded (AUTORCC compiles it in)
    src/main.cpp

    src/app/AppLog.h            src/app/AppLog.cpp
    src/app/AppPaths.h
    src/app/ClothControls.h     src/app/ClothControls.cpp
    src/app/ExportHooks.h
    src/app/ExportSettings.h
    src/app/Hotkeys.h
    src/app/LogConsole.h        src/app/LogConsole.cpp
    src/app/MainWindow.h        src/app/MainWindow.cpp
    src/app/SehGuard.h          src/app/SehGuard.cpp
    src/app/Prereqs.h           src/app/Prereqs.cpp
    src/app/ExportReport.h      src/app/ExportReport.cpp
    src/app/SetsExportPlan.h
    src/app/SetListOptions.h
    src/app/SettingsSchema.h
    src/app/SettingsDialog.h    src/app/SettingsDialog.cpp

    src/gl/GLMeshView.h         src/gl/GLMeshView.cpp
    src/gl/ModelThumbRenderer.h src/gl/ModelThumbRenderer.cpp
    src/gl/GLTextureWidget.h    src/gl/GLTextureWidget.cpp
    src/gl/GifEncoder.h         src/gl/GifEncoder.cpp
    src/app/ExportCapture.h     src/app/ExportCapture.cpp
    src/app/ExportNotifier.h
    src/model/ExportExtras.h    src/model/ExportExtras.cpp

    src/index/AssetIndex.h      src/index/AssetIndex.cpp
    src/index/AssetIndexCache.h src/index/AssetIndexCache.cpp
    src/index/AssetListModel.h  src/index/AssetListModel.cpp
    src/index/AssetOutlinerModel.h src/index/AssetOutlinerModel.cpp
    src/index/ItemNames.h       src/index/ItemNames.cpp
    src/index/NameTranslator.h  src/index/NameTranslator.cpp
    src/index/ThumbnailProvider.h src/index/ThumbnailProvider.cpp

    src/model/AnimParser.h      src/model/AnimParser.cpp
    src/model/AnimPose.h        src/model/AnimPose.cpp
    src/model/ClothSim.h        src/model/ClothSim.cpp
    src/model/GlbExporter.h     src/model/GlbExporter.cpp
    src/model/MaterialParser.h  src/model/MaterialParser.cpp
    src/model/MeshParser.h      src/model/MeshParser.cpp
    src/model/ModelResolve.h    src/model/ModelResolve.cpp
    src/model/SkeletonTreeParser.h src/model/SkeletonTreeParser.cpp
    src/model/SkinSkeletonParser.h src/model/SkinSkeletonParser.cpp

    src/tabs/AssetsTab.h        src/tabs/AssetsTab.cpp
    src/tabs/BatchSink.h
    src/tabs/BulkExtractTab.h   src/tabs/BulkExtractTab.cpp
    src/tabs/ModelsTab.h        src/tabs/ModelsTab.cpp
    src/tabs/TexturesTab.h      src/tabs/TexturesTab.cpp
    src/tabs/WardrobeTab.h      src/tabs/WardrobeTab.cpp

    src/tex/AtlasPlist.h        src/tex/AtlasPlist.cpp
    src/tex/BcDecode.h          src/tex/BcDecode.cpp
    src/tex/DiPixelFormat.h     src/tex/DiPixelFormat.cpp
    src/tex/TextureDecode.h     src/tex/TextureDecode.cpp
    src/tex/TextureParser.h     src/tex/TextureParser.cpp

    src/util/AnimExportScope.h
    src/util/CacheVersioning.h
    src/util/ExportLayout.h
    src/util/NameTemplate.h
    src/util/TexturePreview.h
    src/util/CameraPanel.h      src/util/CameraPanel.cpp
    src/util/CsvCopy.h          src/util/CsvCopy.cpp
    src/util/FilterBar.h        src/util/FilterBar.cpp
    src/util/FilterSpec.h
    src/util/HintBar.h
    src/util/InfoNotes.h
    src/util/HoverInfo.h
    src/util/HoverPreview.h   src/util/HoverPreview.cpp
    src/util/MenuText.h
    src/util/PanelPersist.h)
target_include_directories(DIAssetBrowser PRIVATE src)
# Network is used for exactly one thing: Settings > Prerequisites fetching the
# Microsoft VC++ redistributable when it is missing. The app makes no other
# request, phones nothing home, and works fully offline.
target_link_libraries(DIAssetBrowser PRIVATE di_store Qt6::Core Qt6::Gui Qt6::Widgets
                                             Qt6::OpenGL Qt6::OpenGLWidgets
                                             Qt6::Network)

# SEH guard: /EHa lets structured exceptions become catchable C++ exceptions in
# guarded sections (same setup as the D4 tool). MSVC only.
if(MSVC)
    target_compile_options(DIAssetBrowser PRIVATE /EHa)
endif()

# exe-file icon + version resource (Windows only; no RC compiler elsewhere).
if(WIN32)
    target_sources(DIAssetBrowser PRIVATE res/app.rc)
endif()

# ── install: portable release folder ────────────────────────────────────────
# Dynamic Qt: copy runtime DLLs + plugin families next to the exe so the zip is
# self-contained. NOT qt_generate_deploy_app_script — this vcpkg Qt ships no
# windeployqt.exe, only its qmake .prf (D4-project lesson; run.bat copies the
# same set by hand and has always worked).
#
# THE MSVC RUNTIME. vcpkg's x64-windows triplet builds everything against the
# DYNAMIC CRT, so the exe needs vcruntime140.dll / vcruntime140_1.dll /
# msvcp140.dll. Those ship with the "Visual C++ v14 Redistributable" (the one
# package covering everything built by VS2017 through VS2026),
# which a developer's machine always has and a clean one very often does not —
# so the release zip ran here and died on the first user with
# "vcruntime140_1.dll was not found". Nothing in the packaging copied them.
#
# InstallRequiredSystemLibraries is the supported way to place them beside the
# exe (Microsoft calls this app-local deployment and permits it for exactly this
# case). Static-linking the CRT instead would avoid the question entirely, but
# an /MT executable against /MD Qt DLLs means two C runtimes and two heaps in
# one process — memory freed by the wrong allocator, which is worse than the
# problem being solved.
if(WIN32)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)   # we place them ourselves
    set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)             # UCRT too, for pre-1809 hosts
    include(InstallRequiredSystemLibraries)

    # FALLBACK for a toolset newer than the running CMake knows about.
    # InstallRequiredSystemLibraries maps a known MSVC_TOOLSET_VERSION onto a
    # redist directory; build with a toolset released after your CMake (this
    # project is built with VS2026 / MSVC 14.5x) and the map simply misses,
    # leaving CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS empty with no error. vcvars64
    # exports VCToolsRedistDir pointing at the redist tree of the EXACT toolset
    # in use, so globbing it is not a guess about layout - it is the same folder
    # the module would have found had it known the version.
    if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS AND DEFINED ENV{VCToolsRedistDir})
        file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}" _vc_redist_dir)
        file(GLOB _vc_crt_dirs "${_vc_redist_dir}/x64/Microsoft.VC*.CRT")
        foreach(_d ${_vc_crt_dirs})
            file(GLOB _dlls "${_d}/*.dll")
            list(APPEND CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS ${_dlls})
        endforeach()
        if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
            message(STATUS "Portable CRT: fell back to VCToolsRedistDir "
                           "(${_vc_redist_dir}) - this CMake does not know this "
                           "MSVC toolset.")
        endif()
    endif()

    if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        install(FILES ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION .)
        message(STATUS "Portable CRT: ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}")
    else()
        message(WARNING
            "No MSVC runtime DLLs found to bundle — the release zip will need "
            "the Visual C++ Redistributable installed on the target machine. "
            "The app's Settings > Prerequisites page can fetch it, but the zip "
            "is no longer self-contained.")
    endif()
endif()

if(WIN32)
    set(_qt_bin "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/bin")
    set(_qt_plugins "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/Qt6/plugins")
    install(DIRECTORY "${_qt_bin}/" DESTINATION . FILES_MATCHING PATTERN "*.dll")
    # Only the plugin families a Widgets/OpenGL app loads. platforms is
    # mandatory — without qwindows.dll the exe dies at startup everywhere.
    # tls is REQUIRED for the https download in Settings > Prerequisites: without
    # qopensslbackend.dll every request fails with "TLS initialization failed"
    # and the one network feature in the app is dead in the release build while
    # working perfectly from the build tree.
    foreach(_p platforms imageformats styles iconengines tls networkinformation)
        if(EXISTS "${_qt_plugins}/${_p}")
            install(DIRECTORY "${_qt_plugins}/${_p}" DESTINATION .
                    FILES_MATCHING PATTERN "*.dll")
        endif()
    endforeach()
endif()

install(TARGETS DIAssetBrowser
    RUNTIME DESTINATION .
    BUNDLE  DESTINATION .)
