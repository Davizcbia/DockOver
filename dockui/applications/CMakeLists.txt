list(APPEND CMAKE_PREFIX_PATH "$ENV{QTDIR}" "C:/Qt/6.12.0/msvc2022_64")
find_package(Qt6 REQUIRED COMPONENTS Widgets)
set(target DockOverApplications)
set(output "${CMAKE_BINARY_DIR}/applications-menu")
set(src "${CMAKE_CURRENT_SOURCE_DIR}/../src")
file(MAKE_DIRECTORY "${output}")
configure_file(applications-defaults.ini "${output}/applications-defaults.ini" COPYONLY)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/glass_ps.h"
    COMMAND "${FXC_EXECUTABLE}" /nologo /O3 /WX /T ps_5_0 /E PSMain /Vn g_GlassPS
        /Fh "${CMAKE_CURRENT_BINARY_DIR}/glass_ps.h" "${CMAKE_CURRENT_SOURCE_DIR}/glass.hlsl"
    DEPENDS glass.hlsl VERBATIM)
add_executable(${target} WIN32 main.cpp "${CMAKE_CURRENT_BINARY_DIR}/glass_ps.h" "${src}/app_registry.cpp"
    "${src}/shell_items.cpp" "${src}/liquid_glass.cpp" "${src}/glass_params.cpp" "${src}/json_lite.cpp")
add_dependencies(${target} LiquidDockShaders)
set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${output}" MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${SHADER_OUT}" "${src}")
target_compile_features(${target} PRIVATE cxx_std_20)
target_compile_definitions(${target} PRIVATE QT_NO_KEYWORDS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(${target} PRIVATE /utf-8)
target_link_libraries(${target} PRIVATE Qt6::Widgets d3d11 dxgi dwmapi shell32 propsys ole32 uuid version)
get_target_property(qmake Qt6::qmake IMPORTED_LOCATION)
get_filename_component(qtbin "${qmake}" DIRECTORY)
add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${qtbin}/windeployqt.exe" --no-translations --no-quick-import --no-opengl-sw --no-system-d3d-compiler "$<TARGET_FILE:${target}>" VERBATIM)

