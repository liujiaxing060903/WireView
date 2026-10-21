add_executable(wireview_host
    src/annex_b_stream.cpp
    src/desktop_encoder.cpp
    src/development_transport.cpp
    src/idd_frame_relay.cpp
    src/idd_frame_relay_pipe.cpp
    src/input_injector.cpp
    src/main.cpp
    src/parsec_vdd.cpp
    src/reliable_byte_stream.cpp
    src/usb_accessory_discovery.cpp
    src/winusb_byte_stream.cpp
    src/windows_video.cpp
)

# 保留原生 GDI 控制中心，供未安装 Evergreen Runtime 的旧系统回退使用。
add_executable(wireview_control_legacy WIN32
    src/control_main.cpp
    src/parsec_vdd_custom_modes.cpp
    src/parsec_vdd.cpp
    src/wireview_control.rc
)

# 当前控制中心：同一份经验证的 C++ 后端加 WebView2 展示层。业务逻辑仍由
# control_main.cpp 持有；WEBVIEW 宏只替换其可见的控制中心表面。
add_executable(wireview_control WIN32
    src/control_main.cpp
    src/parsec_vdd_custom_modes.cpp
    src/parsec_vdd.cpp
    src/webview_host.cpp
    src/wireview_control.rc
)

# 原生会话入口：负责 ADB 准备、Android 应用唤起与 Host 重连，不依赖 PowerShell。
add_executable(wireview_session WIN32
    src/session_main.cpp
)

add_executable(wireview_vdd_broker WIN32
    src/parsec_vdd_custom_modes.cpp
    src/parsec_vdd.cpp
    src/vdd_broker_main.cpp
)

add_executable(wireview_usb_probe
    src/usb_accessory_probe.cpp
)

set(WIREVIEW2_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../external/webview2/sdk)
set(WIREVIEW2_INCLUDE_DIR ${WIREVIEW2_SDK_DIR}/build/native/include)
set(WIREVIEW2_LOADER_LIB ${WIREVIEW2_SDK_DIR}/build/native/x64/WebView2LoaderStatic.lib)
if(NOT EXISTS ${WIREVIEW2_INCLUDE_DIR}/WebView2.h OR NOT EXISTS ${WIREVIEW2_LOADER_LIB})
    message(FATAL_ERROR "WebView2 C++ SDK is missing. Restore external/webview2/sdk from Microsoft.Web.WebView2 1.0.4191.47.")
endif()

foreach(control_target IN ITEMS wireview_control wireview_control_legacy)
    target_link_libraries(${control_target} PRIVATE
        setupapi shell32 user32 advapi32 dwmapi uxtheme msimg32 comctl32 gdiplus)
    target_compile_features(${control_target} PRIVATE cxx_std_20)
endforeach()

target_compile_definitions(wireview_control PRIVATE WIREVIEW_WEBVIEW_UI=1)
target_include_directories(wireview_control PRIVATE ${WIREVIEW2_INCLUDE_DIR})
target_link_libraries(wireview_control PRIVATE ${WIREVIEW2_LOADER_LIB} ole32)

# 控制中心使用的主视觉与输入模式图标。保留为独立 PNG，既能避免 16px 图标失真，
# 也方便后续替换为用户提供的设备素材。
set(WIREVIEW_CONTROL_UI_ASSET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets/ui)
function(copy_control_ui_assets target)
    add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/assets/ui"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WIREVIEW_CONTROL_UI_ASSET_DIR}/copy-screen-cutout.png"
        "$<TARGET_FILE_DIR:${target}>/assets/ui/copy-screen.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WIREVIEW_CONTROL_UI_ASSET_DIR}/extend-screen-cutout.png"
        "$<TARGET_FILE_DIR:${target}>/assets/ui/extend-screen.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WIREVIEW_CONTROL_UI_ASSET_DIR}/icon-display-only.png"
        "$<TARGET_FILE_DIR:${target}>/assets/ui/icon-display-only.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WIREVIEW_CONTROL_UI_ASSET_DIR}/icon-touch-pen.png"
        "$<TARGET_FILE_DIR:${target}>/assets/ui/icon-touch-pen.png"
)
endfunction()

copy_control_ui_assets(wireview_control_legacy)
copy_control_ui_assets(wireview_control)

set(WIREVIEW_WEB_ASSET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets/web)
set(WIREVIEW_DEVICE_MODEL_DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../data/device-models)
# Treat web resources as target inputs as well as post-build copy sources.  This
# makes an incremental package pick up a changed HTML/CSS/JS file even when no
# C++ source needed recompiling.
file(GLOB_RECURSE WIREVIEW_WEB_ASSETS CONFIGURE_DEPENDS
    "${WIREVIEW_WEB_ASSET_DIR}/*")
set(WIREVIEW_WEB_ASSET_STAMP "${CMAKE_CURRENT_BINARY_DIR}/wireview-web-assets.stamp")
add_custom_command(
    OUTPUT "${WIREVIEW_WEB_ASSET_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${WIREVIEW_WEB_ASSET_DIR}" "$<TARGET_FILE_DIR:wireview_control>/web"
    COMMAND ${CMAKE_COMMAND} -E touch "${WIREVIEW_WEB_ASSET_STAMP}"
    DEPENDS ${WIREVIEW_WEB_ASSETS}
    COMMENT "Copying WireView web assets"
)
add_custom_target(wireview_web_assets DEPENDS "${WIREVIEW_WEB_ASSET_STAMP}")
add_dependencies(wireview_control wireview_web_assets)
add_custom_command(TARGET wireview_control POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${WIREVIEW_WEB_ASSET_DIR}" "$<TARGET_FILE_DIR:wireview_control>/web"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:wireview_control>/web/ui"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WIREVIEW_CONTROL_UI_ASSET_DIR}/copy-screen-cutout.png"
        "$<TARGET_FILE_DIR:wireview_control>/web/ui/copy-screen.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WIREVIEW_CONTROL_UI_ASSET_DIR}/extend-screen-cutout.png"
        "$<TARGET_FILE_DIR:wireview_control>/web/ui/extend-screen.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WIREVIEW_CONTROL_UI_ASSET_DIR}/icon-display-only.png"
        "$<TARGET_FILE_DIR:wireview_control>/web/ui/icon-display-only.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WIREVIEW_CONTROL_UI_ASSET_DIR}/icon-touch-pen.png"
        "$<TARGET_FILE_DIR:wireview_control>/web/ui/icon-touch-pen.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WIREVIEW_CONTROL_UI_ASSET_DIR}/wireview-app-icon.png"
        "$<TARGET_FILE_DIR:wireview_control>/web/ui/wireview-app-icon.png"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:wireview_control>/data/device-models"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WIREVIEW_DEVICE_MODEL_DATA_DIR}/android-cn-mobile.tsv"
        "$<TARGET_FILE_DIR:wireview_control>/data/device-models/android-cn-mobile.tsv"
)

target_link_libraries(wireview_session PRIVATE user32)
target_compile_features(wireview_session PRIVATE cxx_std_20)

target_link_libraries(wireview_vdd_broker PRIVATE setupapi shell32 user32 advapi32)
target_compile_features(wireview_vdd_broker PRIVATE cxx_std_20)

target_link_libraries(wireview_usb_probe PRIVATE setupapi winusb wireview_protocol)
target_compile_features(wireview_usb_probe PRIVATE cxx_std_20)

target_link_libraries(wireview_host PRIVATE wireview_protocol)
target_include_directories(wireview_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(wireview_host PRIVATE
    d3d11
    d3d10_1
    d3dcompiler
    dxgi
    mf
    mfplat
    mfuuid
    ole32
    shcore
    user32
    ws2_32
    advapi32
    setupapi
    winusb
)
target_compile_features(wireview_host PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(wireview_host PRIVATE /W4 /WX /permissive-)
    target_compile_options(wireview_control PRIVATE /W4 /WX /permissive- /utf-8)
    target_compile_options(wireview_control_legacy PRIVATE /W4 /WX /permissive- /utf-8)
    target_compile_options(wireview_session PRIVATE /W4 /WX /permissive- /utf-8)
    target_compile_options(wireview_vdd_broker PRIVATE /W4 /WX /permissive- /utf-8)
    target_compile_options(wireview_usb_probe PRIVATE /W4 /WX /permissive- /utf-8)
endif()

if(BUILD_TESTING)
    add_executable(wireview_idd_frame_relay_tests
        src/idd_frame_relay.cpp
        tests/idd_frame_relay_tests.cpp
    )
    target_include_directories(wireview_idd_frame_relay_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../common
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(wireview_idd_frame_relay_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(wireview_idd_frame_relay_tests PRIVATE /W4 /WX /permissive-)
    endif()
    add_test(NAME wireview_idd_frame_relay_tests COMMAND wireview_idd_frame_relay_tests)

    add_executable(wireview_idd_frame_relay_pipe_tests
        src/idd_frame_relay.cpp
        src/idd_frame_relay_pipe.cpp
        tests/idd_frame_relay_pipe_tests.cpp
    )
    target_include_directories(wireview_idd_frame_relay_pipe_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../common
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(wireview_idd_frame_relay_pipe_tests PRIVATE cxx_std_20)
    target_link_libraries(
        wireview_idd_frame_relay_pipe_tests PRIVATE d3d11 dxgi user32 advapi32)
    if(MSVC)
        target_compile_options(wireview_idd_frame_relay_pipe_tests PRIVATE /W4 /WX /permissive-)
    endif()
    add_test(
        NAME wireview_idd_frame_relay_pipe_tests
        COMMAND wireview_idd_frame_relay_pipe_tests)
    set_tests_properties(wireview_idd_frame_relay_pipe_tests PROPERTIES TIMEOUT 10)

    add_test(NAME wireview_host_loopback COMMAND wireview_host --loopback)
    add_test(NAME wireview_vdd_broker_self_test COMMAND wireview_vdd_broker --self-test)
    add_test(NAME wireview_control_status_self_test COMMAND wireview_control --self-test-status)
    add_test(NAME wireview_usb_probe_self_test COMMAND wireview_usb_probe --self-test)

    add_executable(wireview_reliable_byte_stream_tests
        src/reliable_byte_stream.cpp
        tests/reliable_byte_stream_tests.cpp
    )
    target_link_libraries(wireview_reliable_byte_stream_tests PRIVATE wireview_protocol)
    target_include_directories(wireview_reliable_byte_stream_tests PRIVATE src)
    target_compile_features(wireview_reliable_byte_stream_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(
            wireview_reliable_byte_stream_tests PRIVATE /W4 /WX /permissive-)
    endif()
    add_test(
        NAME wireview_reliable_byte_stream_tests
        COMMAND wireview_reliable_byte_stream_tests)

    add_executable(wireview_winusb_byte_stream_tests
        src/winusb_byte_stream.cpp
        tests/winusb_byte_stream_tests.cpp
    )
    target_link_libraries(wireview_winusb_byte_stream_tests PRIVATE
        wireview_protocol
        winusb)
    target_include_directories(wireview_winusb_byte_stream_tests PRIVATE src)
    target_compile_features(wireview_winusb_byte_stream_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(
            wireview_winusb_byte_stream_tests PRIVATE /W4 /WX /permissive-)
    endif()
    add_test(
        NAME wireview_winusb_byte_stream_tests
        COMMAND wireview_winusb_byte_stream_tests)

    add_executable(wireview_usb_accessory_discovery_tests
        src/usb_accessory_discovery.cpp
        tests/usb_accessory_discovery_tests.cpp
    )
    target_link_libraries(wireview_usb_accessory_discovery_tests PRIVATE setupapi)
    target_include_directories(wireview_usb_accessory_discovery_tests PRIVATE src)
    target_compile_features(wireview_usb_accessory_discovery_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(
            wireview_usb_accessory_discovery_tests PRIVATE /W4 /WX /permissive-)
    endif()
    add_test(
        NAME wireview_usb_accessory_discovery_tests
        COMMAND wireview_usb_accessory_discovery_tests)
    add_test(
        NAME wireview_invalid_transport
        COMMAND
            ${CMAKE_COMMAND}
            "-DHOST=$<TARGET_FILE:wireview_host>"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid_transport_test.cmake)
    add_test(
        NAME wireview_start_script_transport
        COMMAND
            powershell.exe
            -NoProfile
            -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/start_script_transport_test.ps1
            -Script ${CMAKE_SOURCE_DIR}/scripts/start-wireview.ps1)
    add_test(
        NAME wireview_reconnect_missing_display
        COMMAND
            ${CMAKE_COMMAND}
            "-DHOST=$<TARGET_FILE:wireview_host>"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/reconnect_missing_display_test.cmake)
    set_tests_properties(
        wireview_reconnect_missing_display
        PROPERTIES TIMEOUT 5)
endif()
