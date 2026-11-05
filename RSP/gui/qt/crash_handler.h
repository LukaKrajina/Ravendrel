#pragma once

namespace rvdlrsp::gui
{

// 轻量崩溃处理器：注册 POSIX 信号，崩溃时把堆栈回溯写入日志文件后重新抛出。
// 已为完整 crashpad/breakpad 接入预留 install() 入口（替换实现即可）。
//
// ─── 完整 crashpad/breakpad 接入方案（需外部资源）────────────────────
// 若要生成 minidump 并云端上报，需引入第三方库 + 服务端，步骤如下：
//
// 1) 构建 Breakpad（较简单，仅需 depot_tools + git）：
//      fetch breakpad && cd breakpad && ./configure && make
//    产出 src/client/linux/libbreakpad_client.a。
//
// 2) 初始化（替换本类的 install() 实现）：
//      google_breakpad::MinidumpDescriptor desc(log_dir);
//      auto *handler = new google_breakpad::ExceptionHandler(
//          desc, /*filter*/nullptr,
//          [](const char *dump_path, const char *minidump_id, void*, bool succeeded) {
//              if (succeeded) upload_minidump(std::string(dump_path) + "/" + minidump_id + ".dmp");
//              return succeeded;
//          }, nullptr, true, -1);
//    崩溃时自动写 .dmp 并回调 upload_minidump()。
//
// 3) 云端上报（需服务端 URL）：
//    用 HTTP multipart POST 把 .dmp 上传到崩溃收集服务（如 Sentry/Bugsplat
//    或自建 /crash-report 端点），服务端解析 minidump 符号化后归集。
//
// 4) 符号化（CI 保留带符号的构建产物）：minidump 需要与对应版本的符号表
//    （.sym 文件）配合才能还原可读堆栈。
//
// 因此「minidump + 云端上报」的完整落地依赖：第三方库构建、崩溃收集服务端、
// 符号服务器。当前项目采用轻量 backtrace 方案，接口已预留，待上述资源就绪后
// 替换 crash_handler.cpp 的 install() 即可。

    class CrashHandler
    {
        public:
            static void install(); // 安装信号处理器（幂等，在 main 中 LogManager::init 之后调用）
    };
}