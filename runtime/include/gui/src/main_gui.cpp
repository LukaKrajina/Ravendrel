#include <string>
#include <iostream>
#include "app.h"

#ifdef _WIN32
#include <windows.h>
#include <cstdio>

static LONG WINAPI crash_vectored_handler(EXCEPTION_POINTERS *ep)
{
    if (!ep || !ep->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void *addr = ep->ExceptionRecord->ExceptionAddress;

    FILE *f = fopen("crash_dump.txt", "a");
    if (f)
    {
        HMODULE base = GetModuleHandleW(NULL);
        fprintf(f, "[CRASH] exception 0x%08lX at address %p, module_base %p, rva 0x%llX\n",
                code, addr, base,
                (unsigned long long)((uintptr_t)addr - (uintptr_t)base));
        if (ep->ContextRecord)
        {
            uintptr_t rsp = (uintptr_t)ep->ContextRecord->Rsp;
            uintptr_t *stack = (uintptr_t *)rsp;
            fprintf(f, "[STACK] %p %p %p %p %p %p %p %p\n",
                    (void *)stack[0], (void *)stack[1], (void *)stack[2], (void *)stack[3],
                    (void *)stack[4], (void *)stack[5], (void *)stack[6], (void *)stack[7]);
        }
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

#define DAEMON_HOST "127.0.0.1"
#define DAEMON_PORT 50052

int main() {
#ifdef _WIN32
    AddVectoredExceptionHandler(1, crash_vectored_handler);
#endif
    qgui::App app;
    if (!app.init("Ravendrel QVM Visualizer", 1280, 1080, DAEMON_HOST, DAEMON_PORT)) {
        std::cerr << "[GUI] Failed to initialize visualizer\n";
        return -1;
    }
    app.run();
    app.shutdown();
    return 0;
}