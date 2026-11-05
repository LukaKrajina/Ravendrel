cmake_minimum_required(VERSION 3.25)

# ─── 代码签名函数（standalone 构建时需要）──────────
function(rvdl_sign_target TARGET_NAME)
    if(WIN32)
        find_program(POWERSHELL_EXE powershell)
        if(POWERSHELL_EXE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/sign.ps1")
            add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                COMMAND ${POWERSHELL_EXE} -ExecutionPolicy Bypass -File
                    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/sign.ps1"
                    -Targets "$<TARGET_FILE:${TARGET_NAME}>"
                COMMENT "Code signing ${TARGET_NAME}.exe")
        endif()
    endif()
endfunction()


# ─── Kokkos (standalone) ───
if(NOT TARGET Kokkos::kokkos)
    find_package(Kokkos REQUIRED CONFIG)
endif()

# ─── 原有内容 ──────────────────────────────
# ─── stb_image 检测（vendor/stb/stb_image.h）──────────────
set(STB_IMAGE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/stb)
set(STB_IMAGE_HEADER ${STB_IMAGE_DIR}/stb_image.h)
if(EXISTS ${STB_IMAGE_HEADER})
    set(RVDLRSP_HAS_STB_IMAGE TRUE)
    message(STATUS "stb_image found: ${STB_IMAGE_HEADER}")
else()
    # stb_image 为 vendored 依赖(仓库内已提供),缺失即硬失败,
    # 避免「静默降级」导致 JPEG/PNG 纹理解码在生产环境悄悄失效。
    message(FATAL_ERROR "stb_image.h not found at ${STB_IMAGE_HEADER}. "
                        "This vendored dependency is required for JPEG/PNG texture decoding. "
                        "Restore it by downloading https://github.com/nothings/stb "
                        "(stb_image.h) into vendor/stb/")
endif()

# rvdlRSP
add_executable(rvdlRSP
    src/main.cpp
    src/vulkan_context.cpp
    src/rvdl_jit_host.cpp
)
if(RVDLRSP_HAS_STB_IMAGE)
    target_sources(rvdlRSP PRIVATE src/stb_image_impl.cpp)
    target_compile_definitions(rvdlRSP PRIVATE RVDLRSP_USE_STB_IMAGE)
endif()
target_include_directories(rvdlRSP PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include
    ${VENDOR_DIR}
    ${STB_IMAGE_DIR}
)
target_link_libraries(rvdlRSP PRIVATE rvdl_rt Kokkos::kokkos ${Vulkan_LIBRARIES} ${GLFW_LIB})
if(WIN32)
    # 应用图标（rvdlrsp.ico → 嵌入 rvdlRSP.exe）
    target_sources(rvdlRSP PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../icons/rvdlrsp.rc")
    target_link_libraries(rvdlRSP PRIVATE ws2_32 gdi32)
    # GUI 程序：无控制台窗口
    set_target_properties(rvdlRSP PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_link_options(rvdlRSP PRIVATE /ENTRY:mainCRTStartup)
    endif()
elseif(UNIX AND NOT APPLE)
    target_link_libraries(rvdlRSP PRIVATE m pthread dl)
endif()
rvdl_sign_target(rvdlRSP)

# ─── 着色器编译──────────
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
if(GLSLC)
    set(SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    foreach(s mesh.vert mesh.frag sky.vert sky.frag line.vert line.frag)
        add_custom_command(
            OUTPUT ${SHADER_OUT_DIR}/${s}.spv
            COMMAND ${GLSLC} --target-env=vulkan1.0 ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${s} -o ${SHADER_OUT_DIR}/${s}.spv
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${s}
            COMMENT "Compiling shader ${s}"
        )
    endforeach()
    add_custom_target(rvdlRSP_shaders ALL
        DEPENDS ${SHADER_OUT_DIR}/mesh.vert.spv ${SHADER_OUT_DIR}/mesh.frag.spv
                ${SHADER_OUT_DIR}/sky.vert.spv ${SHADER_OUT_DIR}/sky.frag.spv
                ${SHADER_OUT_DIR}/line.vert.spv ${SHADER_OUT_DIR}/line.frag.spv)
    add_dependencies(rvdlRSP rvdlRSP_shaders)
else()
    message(WARNING "glslc not found — build shaders manually (mesh.vert/mesh.frag -> SPIR-V)")
endif()

# ─── 单元测试 ───────────────────────────────────────────
enable_testing()
add_executable(rvdlRSP_tests
    tests/test_main.cpp
    tests/test_math.cpp
    tests/test_physics.cpp
    tests/test_cylinder_collision.cpp
    tests/test_joints.cpp
    tests/test_skeleton_bind.cpp
    tests/test_robot_json.cpp
    tests/test_chaos_mesh.cpp
    tests/test_mesh_generator.cpp
    tests/test_gate_commutation.cpp
    tests/test_graph.cpp
    tests/test_pcg.cpp
    tests/test_qcdrc.cpp
    tests/test_qpu_circuit.cpp
    tests/test_json.cpp
    tests/test_rl.cpp
    tests/test_device.cpp
    tests/test_glb.cpp
    tests/test_bridge.cpp
    tests/test_consciousness.cpp
    tests/test_rl_pipeline.cpp
    tests/test_mocap_device.cpp
    tests/test_gltf_material.cpp
    tests/test_texture_decoder.cpp
    tests/test_brain_bridge.cpp
    tests/test_gltf_uv.cpp
    tests/test_jpeg_decoder.cpp
    tests/test_field_mesh.cpp
    tests/test_prosthetic.cpp
    tests/test_prosthetic_driver.cpp
    tests/test_hardware.cpp
    tests/test_safety.cpp
    tests/test_hil.cpp
    tests/test_qgfx.cpp          # qgfx_*/qqui_* 量子图形与量子 UI
    tests/test_qrl.cpp          # qrl::* Gymnasium 对齐层（空间 / 包装器 / 向量环境）
    tests/test_terrain.cpp       # 高度场地形（Genesis Terrain 对标）
    tests/test_qgfx_witness.cpp  # 纠缠见证条件（真多体纠缠 / 二可分）
)
target_include_directories(rvdlRSP_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include
    ${VENDOR_DIR}
)
# test_robot_json/test_chaos_mesh经 physics_kernel → QVM 间接依赖 Kokkos
# qgfx_* / qqui_* 的实现位于 rvdl_rt（与 cgfx/cgui 同处一个库）。
# ⚠ 必须显式带 ${GLFW_LIB}：rvdl_rt 对 GLFW 是 **PRIVATE** 链接，符号不传递，
# 而 librvdl_rt.so 里留有 glfw* 的未定义引用 —— 链接可执行文件时不给 GLFW
# 就会报 "undefined reference to glfwGetCursorPos / glfwWindowHint / …"。
# （与 runtime/CMakeLists.txt 里 runtime 目标的处理一致。）
target_link_libraries(rvdlRSP_tests PRIVATE rvdl_rt Kokkos::kokkos ${GLFW_LIB})
add_test(NAME rvdlRSP_tests COMMAND rvdlRSP_tests)

# ─── RL 端到端示例 ─────────────────────────────────────
# 已移至 examples/rl_physics_demo.cpp，由 RVDLRSP_BUILD_EXAMPLES 控制是否构建
option(RVDLRSP_BUILD_EXAMPLES "Build RSP example applications" OFF)

if(RVDLRSP_BUILD_EXAMPLES)
    add_executable(rvdlRSP_rl_physics_demo
        examples/rl_physics_demo.cpp
    )
    target_include_directories(rvdlRSP_rl_physics_demo PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include
        ${VENDOR_DIR}
    )
    target_link_libraries(rvdlRSP_rl_physics_demo PRIVATE rvdl_rt Kokkos::kokkos)
    if(WIN32)
        target_link_libraries(rvdlRSP_rl_physics_demo PRIVATE ws2_32)
    endif()
endif()

# ─── 性能基准测试 ──────────────────────
add_executable(rvdlRSP_benchmark
    src/benchmark.cpp
)
target_include_directories(rvdlRSP_benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include
    ${VENDOR_DIR}
)
target_link_libraries(rvdlRSP_benchmark PRIVATE rvdl_rt Kokkos::kokkos ${GLFW_LIB})
if(WIN32)
    target_link_libraries(rvdlRSP_benchmark PRIVATE ws2_32)
endif()

# ─── Linux rpath：从构建目录 / 自身目录加载 librvdl_rt.so ───────────
if(UNIX AND NOT APPLE)
    set_target_properties(rvdlRSP rvdlRSP_tests rvdlRSP_benchmark
        PROPERTIES
        BUILD_RPATH "$<TARGET_FILE_DIR:rvdl_rt>"
        INSTALL_RPATH "\$ORIGIN")
    if(TARGET rvdlRSP_rl_physics_demo)
        set_target_properties(rvdlRSP_rl_physics_demo PROPERTIES
            BUILD_RPATH "$<TARGET_FILE_DIR:rvdl_rt>"
            INSTALL_RPATH "\$ORIGIN")
    endif()
endif()

# ============================================================
# RVDLRSP GUI —— Qt Widgets 量子机器人仿真平台 UI
# ============================================================
# Qt 6.11.2 MSVC 版位于 "C:/Qt/6.11.2/msvc2022_64"
if(WIN32 AND NOT Qt6_DIR)
    unset(Qt6_DIR CACHE)
    set(Qt6_DIR "C:/Qt/6.11.2/msvc2022_64/lib/cmake/Qt6")
endif()
find_package(Qt6 QUIET COMPONENTS Widgets Gui Multimedia Test Concurrent)
if(Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)

    # ─── 仿真内核静态库（含真实 PhysicsKernel/Teleop/RL/QVM）────────
    # 单独成库并链接 Kokkos，避免 Kokkos 的 CUDA 编译规则污染 Qt 代码
    add_library(rvdlRSP_sim STATIC
        gui/src/simulation_host.cpp
    )
    target_include_directories(rvdlRSP_sim PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include
        ${CMAKE_CURRENT_SOURCE_DIR}/gui/qt
        ${VENDOR_DIR}
    )
    target_link_libraries(rvdlRSP_sim PRIVATE rvdl_rt Kokkos::kokkos)
    # 注入 chaos 测试模型路径（若存在则优先加载，否则回退程序化立方体）
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../_tmp_test.glb")
        target_compile_definitions(rvdlRSP_sim PRIVATE
            RVDLRSP_TEST_GLB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../_tmp_test.glb")
        message(STATUS "chaos mesh: will load _tmp_test.glb")
    endif()

    # ─── Qt GUI 可执行（QVulkanWindow 的真 3D 视口 + QTabWidget 面板）──
    add_executable(rvdlRSP_gui
        gui/qt/main.cpp
        gui/qt/main_window.cpp
        gui/qt/vulkan_viewport.cpp
        gui/qt/panels.cpp
        gui/qt/project_browser.cpp
        gui/qt/loading_screen.cpp
        gui/qt/project_store.cpp
        gui/qt/content_browser.cpp
        gui/qt/script_editor.cpp
        gui/qt/asset_viewers.cpp
        gui/qt/asset_types.cpp
        gui/qt/theme_manager.cpp
        gui/qt/log_manager.cpp
        gui/qt/crash_handler.cpp
        gui/qt/settings_dialog.cpp
        gui/qt/find_replace_dialog.cpp
        gui/qt/i18n/i18n.cpp
        src/rvdl_jit_host.cpp
        resources.qrc
    )
    # ui_utils.h 是手写工具头（非 uic 生成），且项目无 .ui 文件，关闭 AUTOUIC，
    # 避免 AUTOUIC 拦截 #include "ui_utils.h" 去查找不存在的 utils.ui。
    set_target_properties(rvdlRSP_gui PROPERTIES AUTOUIC OFF)
    target_include_directories(rvdlRSP_gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include
        ${VENDOR_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/gui/qt
        ${CMAKE_CURRENT_SOURCE_DIR}/gui/src
    )
    # QVulkanWindow 需要 Qt6::Gui（含 Vulkan 支持）+ Vulkan SDK
    # OpenSSL（libcrypto）：插件包 Ed25519 签名校验
    find_package(OpenSSL QUIET)
    # ${GLFW_LIB} 的理由同 rvdlRSP_tests：rvdl_rt 对 GLFW 是 PRIVATE 链接，
    # 符号不传递；rvdlRSP_sim 是静态库 → 它的 GLFW 未定义引用会落到最终
    # 可执行文件上，所以这里也要显式补。
    target_link_libraries(rvdlRSP_gui PRIVATE Qt6::Widgets Qt6::Gui Qt6::Multimedia Qt6::Concurrent rvdlRSP_sim ${Vulkan_LIBRARIES} ${GLFW_LIB})
    if(OpenSSL_FOUND)
        target_link_libraries(rvdlRSP_gui PRIVATE OpenSSL::Crypto)
    else()
        target_link_libraries(rvdlRSP_gui PRIVATE crypto)
    endif()
    # rvdlRSP_gui 是纯 Qt/Vulkan 代码，必须用 host 编译器（g++）编译。
    # 全局 CXX 编译器是 nvcc_wrapper（供 Kokkos CUDA 用），但它会把 .cpp 也
    # 交给 nvcc，导致 Qt 代码的 STL ABI 与 Qt 库（g++ 编译）不匹配，运行时
    # 触发 free(): invalid pointer 崩溃。--host-only 让 nvcc_wrapper 改用 g++。
    target_compile_options(rvdlRSP_gui PRIVATE --host-only)
    if(WIN32)
        # 应用图标（rvdlrsp.ico → 嵌入 rvdlRSP_gui.exe）
        target_sources(rvdlRSP_gui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../icons/rvdlrsp.rc")
        set_target_properties(rvdlRSP_gui PROPERTIES WIN32_EXECUTABLE TRUE)
        if(MSVC)
            target_link_options(rvdlRSP_gui PRIVATE /ENTRY:mainCRTStartup)
        endif()
    endif()

    # 注入 shader 绝对路径 + 依赖 shader 编译
    # 注意：值不带引号，避免 nvcc_wrapper --host-only 传参时引号错位；
    # 源码侧用字符串化宏把它转成字符串字面量。
    target_compile_definitions(rvdlRSP_gui PRIVATE
        RVDLRSP_SHADER_DIR=${CMAKE_CURRENT_BINARY_DIR}/shaders)
    if(TARGET rvdlRSP_shaders)
        add_dependencies(rvdlRSP_gui rvdlRSP_shaders)
    endif()

    # ─── Qt 运行时部署（windeployqt）──────────────────────────────
    # 自动复制 Qt DLL + 平台插件（qwindows.dll）等到输出目录，
    # 解决「Qt 平台插件没有被初始化」问题。
    if(WIN32)
        find_program(WINDEPLOYQT_EXE windeployqt HINTS
            "C:/Qt/6.11.2/msvc2022_64/bin")
        if(WINDEPLOYQT_EXE)
            add_custom_command(TARGET rvdlRSP_gui POST_BUILD
                COMMAND ${WINDEPLOYQT_EXE}
                    --release --no-translations --no-system-d3d-compiler
                    --no-opengl-sw
                    "$<TARGET_FILE:rvdlRSP_gui>"
                COMMENT "Deploying Qt runtime (windeployqt)")
        else()
            message(WARNING "windeployqt not found — Qt DLLs must be deployed manually")
        endif()

        # ─── 代码签名（解决 WDAC 拦截）────────────────────────────
        # 在 windeployqt 之后签名（签名必须在 DLL 部署后，否则被覆盖）。
        find_program(POWERSHELL_EXE powershell)
        if(POWERSHELL_EXE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/sign.ps1")
            add_custom_command(TARGET rvdlRSP_gui POST_BUILD
                COMMAND ${POWERSHELL_EXE} -ExecutionPolicy Bypass -File
                    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/sign.ps1"
                    -Targets "$<TARGET_FILE:rvdlRSP_gui>"
                COMMENT "Code signing rvdlRSP_gui.exe")
        endif()
    endif()

    # ─── GUI 纯逻辑模块单元测试（Qt Test）────────────────────────
    # 覆盖 LogManager / project_store(JSON 序列化) / asset_types / ThemeManager，
    # 不依赖 render / Vulkan / Kokkos，仅链接 Qt6::Core/Widgets/Test。
    #
    # ⚠ 这个目标**可能编不出来**，而且不是本项目代码的问题：系统 Qt 6.12 的
    #   qrangemodel_impl.h 在 GCC 16 下被判 ill-formed（-Wtemplate-body），
    #   clang++ 23 编得过 —— 而本项目的**默认（CPU）构建就是 clang++**
    #   （见 scripts/build-linux.sh），所以只有「用 GCC 强制构建」（例如 CUDA
    #   版的 nvcc_wrapper）会撞上，而那时整个构建会因为这个**可选的自测目标**
    #   失败。
    #
    #   为什么不写 try_compile 探测：Qt Test 的类需要 moc 生成的代码，而
    #   try_compile 不跑 AUTOMOC → 探测必然假阴性（把好系统也判成编不了）。
    #   所以这里按**已知的不兼容组合**跳过，并把原因说明白。
    option(RVDLRSP_BUILD_GUI_TESTS "Build the Qt GUI self-test target" ON)
    if(NOT RVDLRSP_BUILD_GUI_TESTS)
        message(STATUS "[RSP] 跳过 Qt GUI 自测目标（RVDLRSP_BUILD_GUI_TESTS=OFF）")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
        message(STATUS
            "[RSP] 跳过 Qt GUI 自测目标：GCC ≥ 16 与系统 Qt 6.12 头文件不兼容"
            "（qrangemodel_impl.h 的 -Wtemplate-body）。"
            "本项目默认用 clang++ 构建，不受影响；"
            "如需该目标请用 clang++ 或关掉 RVDLRSP_BUILD_GUI_TESTS。")
    else()
    add_executable(rvdlRSP_gui_tests
        tests/gui_test_main.cpp
        tests/test_log_manager.cpp
        tests/test_project_store.cpp
        tests/test_asset_types.cpp
        tests/test_theme_manager.cpp
        gui/qt/log_manager.cpp
        gui/qt/project_store.cpp
        gui/qt/theme_manager.cpp
        gui/qt/asset_types.cpp
        gui/qt/i18n/i18n.cpp
    )
    target_include_directories(rvdlRSP_gui_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include
        ${CMAKE_CURRENT_SOURCE_DIR}/gui/qt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${VENDOR_DIR}
    )
    target_link_libraries(rvdlRSP_gui_tests PRIVATE Qt6::Core Qt6::Widgets Qt6::Test)
    add_test(NAME rvdlRSP_gui_tests COMMAND rvdlRSP_gui_tests)
    # 纯逻辑测试，headless CI 环境下用 offscreen 平台即可运行
    set_tests_properties(rvdlRSP_gui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    message(STATUS "Qt6 found: rvdlRSP_gui (Qt 仿真平台 UI) enabled")
else()
    message(WARNING "Qt6 not found — rvdlRSP_gui disabled. "
                    "Set Qt6_DIR to C:/Qt/6.11.2/msvc2022_64/lib/cmake/Qt6")
endif()

# ============================================================
# 安装规则: rvdlRSP 可执行文件 + 公共头文件
# ============================================================
install(TARGETS rvdlRSP rvdlRSP_benchmark
        RUNTIME DESTINATION bin)
if(TARGET rvdlRSP_rl_physics_demo)
    install(TARGETS rvdlRSP_rl_physics_demo RUNTIME DESTINATION bin)
endif()
if(TARGET rvdlRSP_gui)
    install(TARGETS rvdlRSP_gui RUNTIME DESTINATION bin)
endif()
install(DIRECTORY include/ DESTINATION include/rvdlRSP)
