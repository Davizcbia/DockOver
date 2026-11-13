cmake_minimum_required(VERSION 3.21)
project(LiquidDock LANGUAGES CXX)
# Static C/C++ runtime: the release folder needs no Visual C++ Redistributable and no runtime DLLs beside the programs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
string(RANDOM LENGTH 32 ALPHABET 0123456789abcdef LIQUID_BUILD_ID)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/liquid_build_id.h" "#pragma once\n#define LIQUID_BUILD_MARKER \"LiquidDesktopBuild:${LIQUID_BUILD_ID}\"\n")
# Software update (Settings > About): the version is the GitHub release tag; see package_docky.ps1.
set(DOCKY_VERSION "0.3.0")  # the GitHub release tag of this version; raise it for every release
set(DOCKY_UPDATE_REPO "Davizcbia/DockOver")  # GitHub repository whose releases carry updates (public)
set(DOCKY_UPDATE_ASSET "DockOver-Windows-Portable")  # name prefix of the release's portable zip
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/docky_version.h.tmp" "#pragma once\n#define DOCKY_VERSION \"${DOCKY_VERSION}\"\n#define DOCKY_UPDATE_REPO \"${DOCKY_UPDATE_REPO}\"\n#define DOCKY_UPDATE_ASSET \"${DOCKY_UPDATE_ASSET}\"\n")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/docky_version.h.tmp" "${CMAKE_CURRENT_BINARY_DIR}/docky_version.h" COPYONLY)

# HLSL is compiled at build time with fxc (Windows SDK) into bytecode headers; the Dock never
# compiles shaders at runtime, so parameter changes never trigger shader compilation.
find_program(FXC_EXECUTABLE fxc REQUIRED)
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
function(add_shader src entry profile var header)
    add_custom_command(
        OUTPUT ${SHADER_OUT}/${header}
        COMMAND ${FXC_EXECUTABLE} /nologo /O3 /WX /T ${profile} /E ${entry} /Vn ${var}
                /Fh ${SHADER_OUT}/${header} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${src}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${src}
        COMMENT "fxc ${src} (${entry})"
        VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${SHADER_OUT}/${header} PARENT_SCOPE)
endfunction()
add_shader(glass.hlsl VSMain vs_5_0 g_GlassVS glass_vs.h)
add_shader(glass.hlsl PSMain ps_5_0 g_GlassPS glass_ps.h)
add_shader(blur.hlsl PSDown ps_5_0 g_BlurDownPS blur_down_ps.h)
add_shader(blur.hlsl PSBlur ps_5_0 g_BlurPS blur_ps.h)
add_shader(roi_compare.hlsl CSMain cs_5_0 g_RoiCompareCS roi_compare_cs.h)
add_shader(genie.hlsl VSMain vs_5_0 g_GenieVS genie_vs.h)
add_shader(genie.hlsl PSMain ps_5_0 g_GeniePS genie_ps.h)
add_custom_target(LiquidDockShaders DEPENDS ${SHADER_HEADERS})

add_executable(LiquidDock WIN32
    src/main.cpp
    src/minimize_dock.cpp
    src/genie_gpu.cpp
    src/jump_list.cpp
    src/dock_model.cpp
    src/downloads_stack.cpp
    src/fan_gpu.cpp
    src/unified_menu.cpp
    src/menu_surface.cpp
    src/renderer.cpp
    src/shell_items.cpp
    src/custom_icons.cpp
    src/icon_editor.cpp
    src/material.cpp
    src/liquid_glass.cpp
    src/backdrop_sim.cpp
    src/backdrop_wgc.cpp
    src/glass_params.cpp
    src/json_lite.cpp
    src/glass_studio.cpp
    src/app_registry.cpp
    src/launchpad_model.cpp
    src/launchpad.cpp)
add_dependencies(LiquidDock LiquidDockShaders)
target_include_directories(LiquidDock PRIVATE ${SHADER_OUT})
target_compile_features(LiquidDock PRIVATE cxx_std_20)
target_compile_definitions(LiquidDock PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(LiquidDock PRIVATE /utf-8 /W4 /permissive-)
endif()
target_link_libraries(LiquidDock PRIVATE gdiplus d3d11 dxgi dcomp d2d1 dwrite dwmapi dxguid shell32 ole32 propsys version uuid
    windowscodecs comctl32 comdlg32 windowsapp runtimeobject bcrypt imm32)

# Separate read-only diagnostic; does not alter or launch LiquidDock.
add_library(LiquidTaskbarStatusCore STATIC src/taskbar_status.cpp src/taskbar_snapshot.cpp src/taskbar_ownership.cpp)
target_compile_features(LiquidTaskbarStatusCore PUBLIC cxx_std_20)
target_compile_definitions(LiquidTaskbarStatusCore PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(LiquidTaskbarStatusCore PUBLIC src)
target_link_libraries(LiquidTaskbarStatusCore PUBLIC shell32 user32 ole32)
add_executable(LiquidTaskbarStatus tools/taskbar_status_main.cpp)
target_link_libraries(LiquidTaskbarStatus PRIVATE LiquidTaskbarStatusCore)

add_executable(LiquidTaskbarOwnershipTest tests/taskbar_ownership_test.cpp)
target_link_libraries(LiquidTaskbarOwnershipTest PRIVATE LiquidTaskbarStatusCore)

# Taskbar V0.2 preparation: window-level hide planning (read-only, no window changes).
add_executable(LiquidTaskbarWindowTest tests/taskbar_window_test.cpp src/taskbar_window.cpp)
target_compile_features(LiquidTaskbarWindowTest PRIVATE cxx_std_20)
target_include_directories(LiquidTaskbarWindowTest PRIVATE src)
target_compile_definitions(LiquidTaskbarWindowTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(LiquidTaskbarWindowTest PRIVATE user32 shell32 ole32)

add_executable(DockyFrameReadyTest tests/frame_ready_test.cpp)
target_compile_features(DockyFrameReadyTest PRIVATE cxx_std_20)
target_compile_definitions(DockyFrameReadyTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DockyFrameReadyTest PRIVATE src)

add_executable(DockyLaunchpadDensityTest tests/launchpad_density_test.cpp)
target_compile_features(DockyLaunchpadDensityTest PRIVATE cxx_std_20)
target_include_directories(DockyLaunchpadDensityTest PRIVATE src)

add_executable(DockyDisplayGeometryTest tests/display_geometry_test.cpp)
target_compile_features(DockyDisplayGeometryTest PRIVATE cxx_std_20)
target_compile_definitions(DockyDisplayGeometryTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DockyDisplayGeometryTest PRIVATE src)
target_link_libraries(DockyDisplayGeometryTest PRIVATE user32)

add_executable(DockyMenuNavigationTest tests/menu_navigation_test.cpp)
target_compile_features(DockyMenuNavigationTest PRIVATE cxx_std_20)
target_compile_definitions(DockyMenuNavigationTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DockyMenuNavigationTest PRIVATE src)

# Isolated process-handle watchdog protocol probe; never touches the shell or taskbar.
add_executable(DockyTaskbarGuardProbeTest tests/taskbar_guard_probe_test.cpp)
target_compile_features(DockyTaskbarGuardProbeTest PRIVATE cxx_std_20)
target_compile_definitions(DockyTaskbarGuardProbeTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DockyTaskbarGuardProbeTest PRIVATE src)

# Independent conservative manual recovery; no Dock or Manager process required.
add_executable(DockyTaskbarRestore tools/taskbar_restore.cpp src/taskbar_window.cpp src/taskbar_guardian.cpp src/taskbar_workarea.cpp)
set_target_properties(DockyTaskbarRestore PROPERTIES OUTPUT_NAME docky-taskbar-restore)
target_compile_features(DockyTaskbarRestore PRIVATE cxx_std_20)
target_compile_definitions(DockyTaskbarRestore PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DockyTaskbarRestore PRIVATE src)
target_link_libraries(DockyTaskbarRestore PRIVATE user32 shell32 ole32)

# Simulation-only conflict prompt; cannot alter the Windows taskbar.
add_executable(LiquidTaskbarConflictDemo tools/taskbar_conflict_demo.cpp)
target_link_libraries(LiquidTaskbarConflictDemo PRIVATE LiquidTaskbarStatusCore)

# Independent manager prototype; no taskbar state modification or autostart.
add_executable(DockyTaskbarGuardSim tools/taskbar_guard_sim_main.cpp src/taskbar_guard_sim.cpp src/taskbar_window.cpp)
set_target_properties(DockyTaskbarGuardSim PROPERTIES OUTPUT_NAME docky-taskbar-guard-sim)
target_compile_features(DockyTaskbarGuardSim PRIVATE cxx_std_20)
target_compile_definitions(DockyTaskbarGuardSim PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DockyTaskbarGuardSim PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(DockyTaskbarGuardSim PRIVATE user32 shell32 ole32)

add_executable(LiquidDesktopManager src/desktop_manager.cpp src/taskbar_window.cpp src/taskbar_guard_sim.cpp
               src/taskbar_controller.cpp src/taskbar_guardian.cpp src/taskbar_workarea.cpp)
target_link_libraries(LiquidDesktopManager PRIVATE LiquidTaskbarStatusCore)

# Production taskbar recovery guardian (started by the Manager before any hide; restores after a Manager crash).
add_executable(DockyTaskbarGuardian tools/taskbar_guardian_main.cpp src/taskbar_guardian.cpp src/taskbar_window.cpp src/taskbar_workarea.cpp)
set_target_properties(DockyTaskbarGuardian PROPERTIES OUTPUT_NAME docky-taskbar-guardian)
target_compile_features(DockyTaskbarGuardian PRIVATE cxx_std_20)
target_compile_definitions(DockyTaskbarGuardian PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DockyTaskbarGuardian PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(DockyTaskbarGuardian PRIVATE user32 shell32 ole32)

# Controller state machine, hotkey validation and settings persistence against simulated taskbar windows.
add_executable(DockyTaskbarControllerTest tests/taskbar_controller_test.cpp src/taskbar_controller.cpp src/taskbar_guardian.cpp src/taskbar_workarea.cpp
               src/taskbar_window.cpp src/desktop_settings.cpp src/json_lite.cpp)
target_compile_features(DockyTaskbarControllerTest PRIVATE cxx_std_20)
target_compile_definitions(DockyTaskbarControllerTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DockyTaskbarControllerTest PRIVATE src)
target_link_libraries(DockyTaskbarControllerTest PRIVATE user32 shell32 ole32)

# Real guardian process: handshake, clean stop, crash recovery and corrupt/Pending refusal (isolated records,
# windows that are not taskbars, so no real taskbar is touched).
add_executable(DockyTaskbarGuardianTest tests/taskbar_guardian_test.cpp src/taskbar_window.cpp src/taskbar_guardian.cpp src/taskbar_workarea.cpp)
target_compile_features(DockyTaskbarGuardianTest PRIVATE cxx_std_20)
target_compile_definitions(DockyTaskbarGuardianTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DockyTaskbarGuardianTest PRIVATE src)
target_link_libraries(DockyTaskbarGuardianTest PRIVATE user32 shell32 ole32)

add_executable(LiquidCustomIconsTest tests/custom_icons_test.cpp src/custom_icons.cpp src/json_lite.cpp)
target_compile_features(LiquidCustomIconsTest PRIVATE cxx_std_20)
target_compile_definitions(LiquidCustomIconsTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(LiquidCustomIconsTest PRIVATE src)
target_link_libraries(LiquidCustomIconsTest PRIVATE windowscodecs bcrypt ole32 uuid d2d1 d3d11 dxgi)
add_executable(DockySvgIconTest tests/svg_icon_test.cpp src/custom_icons.cpp src/json_lite.cpp)
target_compile_features(DockySvgIconTest PRIVATE cxx_std_20)
target_compile_definitions(DockySvgIconTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DockySvgIconTest PRIVATE src)
target_link_libraries(DockySvgIconTest PRIVATE windowscodecs bcrypt ole32 uuid d2d1 d3d11 dxgi)
if(MSVC)
    target_compile_options(LiquidCustomIconsTest PRIVATE /utf-8)
endif()

# Standalone, non-activating Menu Bar prototype.
set(LIQUID_MENU_SURFACE_SOURCES src/menu_surface.cpp src/renderer.cpp src/material.cpp src/liquid_glass.cpp src/backdrop_wgc.cpp src/glass_params.cpp src/json_lite.cpp src/dock_model.cpp)
add_executable(LiquidMenuBar WIN32 src/menu_bar.cpp src/native_menu.cpp src/system_status.cpp src/unified_menu.cpp src/system_controls.cpp src/control_center.cpp ${LIQUID_MENU_SURFACE_SOURCES})
add_custom_command(TARGET LiquidMenuBar POST_BUILD
 COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets/user-sound" "$<TARGET_FILE_DIR:LiquidMenuBar>/assets/user-sound")
add_dependencies(LiquidMenuBar LiquidDockShaders)
target_include_directories(LiquidMenuBar PRIVATE ${SHADER_OUT})
target_compile_features(LiquidMenuBar PRIVATE cxx_std_20)
target_compile_definitions(LiquidMenuBar PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(LiquidMenuBar PRIVATE gdiplus user32 gdi32 shell32 advapi32 wlanapi ole32 oleaut32 wbemuuid uuid d3d11 dxgi dcomp d2d1 dwrite dwmapi dxguid windowscodecs windowsapp runtimeobject)
if(MSVC)
    target_compile_options(LiquidMenuBar PRIVATE /utf-8 /W4 /bigobj)
endif()

add_executable(LiquidNativeMenuTest tests/native_menu_test.cpp src/native_menu.cpp)
target_compile_features(LiquidNativeMenuTest PRIVATE cxx_std_20)
target_include_directories(LiquidNativeMenuTest PRIVATE src)
target_compile_definitions(LiquidNativeMenuTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(LiquidNativeMenuTest PRIVATE user32 advapi32)
if(MSVC)
    target_compile_options(LiquidNativeMenuTest PRIVATE /utf-8 /W4)
endif()

add_executable(LiquidDragReorderTest tests/drag_reorder_test.cpp)
target_compile_features(LiquidDragReorderTest PRIVATE cxx_std_20)
target_include_directories(LiquidDragReorderTest PRIVATE src)

add_executable(LiquidDownloadsStackTest tests/downloads_stack_test.cpp src/downloads_stack.cpp)
target_compile_features(LiquidDownloadsStackTest PRIVATE cxx_std_20)
target_compile_definitions(LiquidDownloadsStackTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(LiquidDownloadsStackTest PRIVATE src)
target_link_libraries(LiquidDownloadsStackTest PRIVATE user32 gdi32 shell32 ole32 uuid comctl32 gdiplus dwmapi d2d1)
if(MSVC)
 target_compile_options(LiquidDownloadsStackTest PRIVATE /utf-8 /W4 /permissive-)
endif()

add_executable(LiquidSystemStatusTest tests/system_status_test.cpp src/system_status.cpp)
target_compile_features(LiquidSystemStatusTest PRIVATE cxx_std_20)
target_include_directories(LiquidSystemStatusTest PRIVATE src)
target_compile_definitions(LiquidSystemStatusTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(LiquidSystemStatusTest PRIVATE wlanapi ole32 uuid)
if(MSVC)
 target_compile_options(LiquidSystemStatusTest PRIVATE /utf-8 /W4 /permissive-)
endif()

add_executable(LiquidUnifiedMenuTest tests/unified_menu_test.cpp src/unified_menu.cpp src/native_menu.cpp ${LIQUID_MENU_SURFACE_SOURCES})
add_dependencies(LiquidUnifiedMenuTest LiquidDockShaders)
target_include_directories(LiquidUnifiedMenuTest PRIVATE ${SHADER_OUT})
target_compile_features(LiquidUnifiedMenuTest PRIVATE cxx_std_20)
target_compile_definitions(LiquidUnifiedMenuTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX LIQUID_MENU_DIAGNOSTICS)
target_include_directories(LiquidUnifiedMenuTest PRIVATE src)
target_link_libraries(LiquidUnifiedMenuTest PRIVATE user32 gdi32 d3d11 dxgi dcomp d2d1 dwrite dwmapi dxguid shell32 ole32 propsys version uuid windowscodecs comctl32 comdlg32 windowsapp runtimeobject bcrypt)
if(MSVC)
 target_compile_options(LiquidUnifiedMenuTest PRIVATE /utf-8 /W4 /permissive-)
endif()

# Launchpad layout model + app registry helpers (headless; --discover reads the real AppsFolder).
add_executable(LiquidLaunchpadModelTest tests/launchpad_model_test.cpp src/launchpad_model.cpp src/app_registry.cpp src/json_lite.cpp)
target_compile_features(LiquidLaunchpadModelTest PRIVATE cxx_std_20)
target_compile_definitions(LiquidLaunchpadModelTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(LiquidLaunchpadModelTest PRIVATE src)
target_link_libraries(LiquidLaunchpadModelTest PRIVATE shell32 ole32 uuid)
if(MSVC)
 target_compile_options(LiquidLaunchpadModelTest PRIVATE /utf-8 /W4 /permissive-)
endif()

# Control Center system layer on the real machine (volume/mute changed and restored; radios untouched).
add_executable(LiquidSystemControlsTest tests/system_controls_test.cpp src/system_controls.cpp)
target_compile_features(LiquidSystemControlsTest PRIVATE cxx_std_20)
target_compile_definitions(LiquidSystemControlsTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(LiquidSystemControlsTest PRIVATE src)
target_link_libraries(LiquidSystemControlsTest PRIVATE wlanapi ole32 oleaut32 uuid wbemuuid shell32 windowsapp runtimeobject)
if(MSVC)
 target_compile_options(LiquidSystemControlsTest PRIVATE /utf-8 /W4 /permissive- /bigobj)
endif()

foreach(target LiquidDock LiquidMenuBar LiquidDesktopManager)
 target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()

add_library(LiquidDesktopSettings STATIC src/desktop_settings.cpp src/json_lite.cpp)
target_compile_features(LiquidDesktopSettings PUBLIC cxx_std_20)
target_compile_definitions(LiquidDesktopSettings PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(LiquidDesktopSettings PUBLIC src)
target_link_libraries(LiquidDesktopSettings PUBLIC shell32 ole32)
target_link_libraries(LiquidDesktopManager PRIVATE LiquidDesktopSettings)
# Dock and Menu Bar already compile json_lite.cpp; use the settings source directly there.
target_sources(LiquidDock PRIVATE src/desktop_settings.cpp)
target_sources(LiquidMenuBar PRIVATE src/desktop_settings.cpp)
add_executable(LiquidDesktopSettingsTest tests/desktop_settings_test.cpp)
target_link_libraries(LiquidDesktopSettingsTest PRIVATE LiquidDesktopSettings)

add_executable(LiquidDesktopSettingsApp WIN32 src/settings_window.cpp src/taskbar_guardian.cpp src/taskbar_window.cpp src/taskbar_workarea.cpp src/mydockfinder_import.cpp src/app_registry.cpp src/launchpad_model.cpp)
set_target_properties(LiquidDesktopSettingsApp PROPERTIES OUTPUT_NAME LiquidDesktopSettings)
target_compile_features(LiquidDesktopSettingsApp PRIVATE cxx_std_20)
target_compile_definitions(LiquidDesktopSettingsApp PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(LiquidDesktopSettingsApp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(LiquidDesktopSettingsApp PRIVATE LiquidDesktopSettings d2d1 dwrite dwmapi shell32 advapi32)

# Settings reuses the same GPU material, renderer, shader bytecode and WGC source as menus.
target_sources(LiquidDesktopSettingsApp PRIVATE ${LIQUID_MENU_SURFACE_SOURCES})
add_dependencies(LiquidDesktopSettingsApp LiquidDockShaders)
target_include_directories(LiquidDesktopSettingsApp PRIVATE ${SHADER_OUT})
target_link_libraries(LiquidDesktopSettingsApp PRIVATE d3d11 dxgi dcomp dxguid ole32 propsys version uuid windowscodecs comctl32 comdlg32 windowsapp runtimeobject bcrypt)

if(MSVC)
 target_compile_options(LiquidDesktopSettingsApp PRIVATE /utf-8)
endif()

add_executable(LiquidIconMaskTest tests/icon_mask_test.cpp)
target_compile_features(LiquidIconMaskTest PRIVATE cxx_std_20)
target_include_directories(LiquidIconMaskTest PRIVATE src)

add_executable(LiquidDownloadsFanLayoutTest tests/downloads_fan_layout_test.cpp)
target_compile_features(LiquidDownloadsFanLayoutTest PRIVATE cxx_std_20)
target_include_directories(LiquidDownloadsFanLayoutTest PRIVATE src)

add_executable(DockyMotionTest tests/motion_test.cpp)
target_compile_features(DockyMotionTest PRIVATE cxx_std_20)
target_include_directories(DockyMotionTest PRIVATE src)

add_executable(LiquidDockVisualModelTest tests/dock_model_visual_test.cpp src/dock_model.cpp)
target_compile_features(LiquidDockVisualModelTest PRIVATE cxx_std_20)
target_include_directories(LiquidDockVisualModelTest PRIVATE src)

# Explicit opt-in, read-only diagnostics for windows owned by adjacent docky binaries.
add_executable(DockyGuiDiagnostics tools/gui_diagnostics.cpp)
set_target_properties(DockyGuiDiagnostics PROPERTIES OUTPUT_NAME docky-gui-diagnostics)
target_compile_features(DockyGuiDiagnostics PRIVATE cxx_std_20)
target_compile_definitions(DockyGuiDiagnostics PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(DockyGuiDiagnostics PRIVATE user32)

add_executable(DockyMenuHitRegionTest tests/menu_hit_region_test.cpp)
target_compile_features(DockyMenuHitRegionTest PRIVATE cxx_std_20)
target_compile_definitions(DockyMenuHitRegionTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DockyMenuHitRegionTest PRIVATE src)
target_link_libraries(DockyMenuHitRegionTest PRIVATE user32 gdi32)

# Local static-only Workshop pack storage; no packaged executable code.
add_library(DockyWorkshop STATIC src/workshop.cpp)
target_compile_features(DockyWorkshop PUBLIC cxx_std_20)
target_compile_definitions(DockyWorkshop PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DockyWorkshop PUBLIC src)
target_link_libraries(DockyWorkshop PUBLIC shell32 ole32 windowscodecs uuid)
add_executable(DockyWorkshopTest tests/workshop_test.cpp src/json_lite.cpp)
target_compile_features(DockyWorkshopTest PRIVATE cxx_std_20)
target_compile_definitions(DockyWorkshopTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DockyWorkshopTest PRIVATE src)
target_link_libraries(DockyWorkshopTest PRIVATE DockyWorkshop)
target_link_libraries(LiquidMenuBar PRIVATE DockyWorkshop)
# Wi-Fi network list without package identity (see src/wifi_list.h); never gets the sparse-identity manifest.
add_executable(docky-wifi-list src/wifi_list_main.cpp)
target_compile_features(docky-wifi-list PRIVATE cxx_std_20)
target_compile_definitions(docky-wifi-list PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(docky-wifi-list PRIVATE wlanapi ole32)
if(MSVC)
  target_link_options(docky-wifi-list PRIVATE /SUBSYSTEM:CONSOLE)
endif()
add_dependencies(LiquidMenuBar docky-wifi-list)
target_link_libraries(LiquidDock PRIVATE DockyWorkshop)
target_link_libraries(LiquidDesktopSettingsApp PRIVATE DockyWorkshop)

# Reuse the existing PNG/ICO decoder for imported Menu Bar assets.
target_sources(LiquidMenuBar PRIVATE src/custom_icons.cpp)
target_link_libraries(LiquidMenuBar PRIVATE bcrypt)

add_executable(DockyBatteryIndicatorTest tests/battery_indicator_test.cpp)
target_compile_features(DockyBatteryIndicatorTest PRIVATE cxx_std_20)
target_compile_definitions(DockyBatteryIndicatorTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DockyBatteryIndicatorTest PRIVATE src)
target_link_libraries(DockyBatteryIndicatorTest PRIVATE gdiplus)

# Optional sparse-package identity. Ordinary unpackaged builds continue to work unchanged.
option(DOCKY_SPARSE_IDENTITY "Embed local sparse-package identity" OFF)
if(DOCKY_SPARSE_IDENTITY AND MSVC)
 target_sources(LiquidDesktopManager PRIVATE identity/Manager.manifest)
 target_sources(LiquidDock PRIVATE identity/Dock.manifest)
 target_sources(LiquidMenuBar PRIVATE identity/MenuBar.manifest)
 target_sources(LiquidDesktopSettingsApp PRIVATE identity/Settings.manifest)
endif()

# ---- Menu Bar extras V0.1: configurable right side, Hidden Icons (tray host), extension status items ----
target_sources(LiquidMenuBar PRIVATE src/tray_host.cpp src/menubar_extras.cpp)
add_executable(DockyExtensionsTest tests/extensions_test.cpp src/menubar_extras.cpp src/tray_host.cpp src/json_lite.cpp)
target_compile_features(DockyExtensionsTest PRIVATE cxx_std_20)
target_compile_definitions(DockyExtensionsTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DockyExtensionsTest PRIVATE src)
target_link_libraries(DockyExtensionsTest PRIVATE user32 gdi32 shell32 ole32)
if(MSVC)
  target_compile_options(DockyExtensionsTest PRIVATE /utf-8 /W4)
endif()

# ---- docky Desktop: desktop widgets and file Stacks. A separate program (own exe, own single instance, own settings
# in %LOCALAPPDATA%\DockyDesktop); docky's Desktop Manager never starts it. Links to docky only through the extension
# protocol (src/ext_protocol.h). Its menus reuse the unified popup with capture disabled (opaque docky surface).
add_executable(DockyDesktop WIN32 src/desktop/desktop_main.cpp src/desktop/canvas.cpp src/desktop/widgets.cpp src/desktop/stacks.cpp src/desktop/glass_window.cpp src/desktop/calendar_feed.cpp src/desktop/devices.cpp src/desktop/hid_battery.cpp src/desktop/weather.cpp
  src/unified_menu.cpp src/native_menu.cpp src/desktop_settings.cpp ${LIQUID_MENU_SURFACE_SOURCES})
add_dependencies(DockyDesktop LiquidDockShaders)
set_target_properties(DockyDesktop PROPERTIES OUTPUT_NAME DockOverDesktop)  # the product name (data stays in %LOCALAPPDATA%\DockyDesktop)
target_include_directories(DockyDesktop PRIVATE src ${SHADER_OUT})
target_compile_features(DockyDesktop PRIVATE cxx_std_20)
target_compile_definitions(DockyDesktop PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(DockyDesktop PRIVATE winhttp setupapi hid xinput urlmon user32 gdi32 shell32 shcore ole32 oleaut32 uuid advapi32 d3d11 dxgi dcomp d2d1 dwrite dwmapi dxguid windowscodecs windowsapp runtimeobject winmm)
if(MSVC)
  target_compile_options(DockyDesktop PRIVATE /utf-8 /W4 /bigobj)
  target_link_options(DockyDesktop PRIVATE /MAP)
endif()

# docky-ext: command-line bridge for scripts (send / serve / gallery). Uses only the public SDK header.
add_executable(docky-ext tools/docky_ext_cli.cpp)
target_compile_features(docky-ext PRIVATE cxx_std_20)
target_compile_definitions(docky-ext PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(docky-ext PRIVATE user32)
if(MSVC)
  target_compile_options(docky-ext PRIVATE /utf-8 /W4)
  target_link_options(docky-ext PRIVATE /SUBSYSTEM:CONSOLE)
endif()

# ---- Software update: check/download (Settings, Manager) and the installer that replaces the program files ----
target_sources(LiquidDesktopSettingsApp PRIVATE src/update_client.cpp)
target_link_libraries(LiquidDesktopSettingsApp PRIVATE winhttp)
target_sources(LiquidDesktopManager PRIVATE src/update_client.cpp)
target_link_libraries(LiquidDesktopManager PRIVATE winhttp bcrypt user32 shell32)
add_executable(docky-updater WIN32 tools/updater_main.cpp)
target_compile_features(docky-updater PRIVATE cxx_std_20)
target_compile_definitions(docky-updater PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(docky-updater PRIVATE user32 shell32)
if(MSVC)
  target_compile_options(docky-updater PRIVATE /utf-8 /W4)
endif()
add_dependencies(LiquidDesktopSettingsApp docky-updater)
add_executable(DockyUpdateVersionTest tests/update_version_test.cpp src/update_client.cpp src/json_lite.cpp)
target_compile_features(DockyUpdateVersionTest PRIVATE cxx_std_20)
target_compile_definitions(DockyUpdateVersionTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DockyUpdateVersionTest PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(DockyUpdateVersionTest PRIVATE winhttp bcrypt)

# ---- Version resources (FileDescription is the name Windows shows: Task Manager, notification settings, the Dock) ----
string(REPLACE "." ";" DOCKY_VERSION_PARTS "${DOCKY_VERSION}")
list(APPEND DOCKY_VERSION_PARTS 0 0 0)
list(GET DOCKY_VERSION_PARTS 0 DOCKY_V0)
list(GET DOCKY_VERSION_PARTS 1 DOCKY_V1)
list(GET DOCKY_VERSION_PARTS 2 DOCKY_V2)
function(docky_version_resource target description)
  set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
  file(WRITE "${rc}.tmp" "#include <winver.h>
VS_VERSION_INFO VERSIONINFO
FILEVERSION ${DOCKY_V0},${DOCKY_V1},${DOCKY_V2},0
PRODUCTVERSION ${DOCKY_V0},${DOCKY_V1},${DOCKY_V2},0
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_APP
BEGIN
  BLOCK \"StringFileInfo\"
  BEGIN
    BLOCK \"040904B0\"
    BEGIN
      VALUE \"CompanyName\", \"DockOver\"
      VALUE \"FileDescription\", \"${description}\"
      VALUE \"FileVersion\", \"${DOCKY_VERSION}\"
      VALUE \"ProductName\", \"DockOver\"
      VALUE \"ProductVersion\", \"${DOCKY_VERSION}\"
    END
  END
  BLOCK \"VarFileInfo\"
  BEGIN
    VALUE \"Translation\", 0x409, 1200
  END
END
")
  configure_file("${rc}.tmp" "${rc}" COPYONLY)
  target_sources(${target} PRIVATE "${rc}")
endfunction()
docky_version_resource(LiquidDesktopManager "DockOver")
docky_version_resource(LiquidDock "DockOver Dock")
docky_version_resource(LiquidMenuBar "DockOver Menu Bar")
docky_version_resource(LiquidDesktopSettingsApp "DockOver Settings")
docky_version_resource(DockyDesktop "DockOver Desktop")
docky_version_resource(docky-updater "DockOver Updater")
docky_version_resource(DockyTaskbarGuardian "DockOver Taskbar Guardian")

# ---- Release launcher: DockOver.exe in a release folder starts runtime\LiquidDesktopManager.exe (package_docky.ps1 -Release) ----
add_executable(DockOverLauncher WIN32 tools/launcher_main.cpp)
set_target_properties(DockOverLauncher PROPERTIES OUTPUT_NAME DockOver)
target_compile_features(DockOverLauncher PRIVATE cxx_std_20)
target_compile_definitions(DockOverLauncher PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(DockOverLauncher PRIVATE user32 shell32)
docky_version_resource(DockOverLauncher "DockOver")
