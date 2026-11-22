#include "qcos/Serial.hpp"
#include "qcos/Arch.hpp"
#include "qcos/Idt.hpp"
#include "qcos/Irq.hpp"
#include "qcos/Timer.hpp"
#include "qcos/PreemptSched.hpp"

extern "C" void* memset(void* dst, int c, size_t n)
{
    auto* d = static_cast<unsigned char*>(dst);
    for (size_t i = 0; i < n; ++i) d[i] = static_cast<unsigned char>(c);
    return dst;
}

void operator delete(void*) noexcept { }
void operator delete(void*, size_t) noexcept { }

extern "C" void __cxa_pure_virtual()
{
    qcos::halt();
}

extern "C" const qcos::u64 isr_table[256];
extern "C" void qcos_start_scheduler(qcos::u64 first_rsp);

static qcos::Idt         g_idt;
static qcos::DragonSched g_sched;

alignas(16) static unsigned char g_stack_a[4096];
alignas(16) static unsigned char g_stack_b[4096];
static qcos::Thread g_thread_a;
static qcos::Thread g_thread_b;

alignas(16) static unsigned char g_stack_idle[4096];
static qcos::Thread g_thread_idle;

extern "C" qcos::u64 qcos_interrupt_dispatch(qcos::InterruptFrame* frame)
{
    using namespace qcos;

    Thread* cur = g_sched.current();
    if (cur) cur->kernel_rsp = reinterpret_cast<u64>(frame);

    const u8 vector = static_cast<u8>(frame->vector);
    if (vector >= 0x20 && vector < 0x30)
    {
        const u8 irq = vector - 0x20;
        if (irq == 0)
            g_sched.tick();
        pic_eoi(irq);
    }
    else
    {
        Serial::puts("QCOS: exception!\n");
        halt();
    }

    Thread* next = g_sched.current();
    if (!next) next = g_sched.pick_next();
    if (!next) halt();
    next->state = ThreadState::RUNNING;

    return next->kernel_rsp;
}

static void task_idle(void*)
{
    for (;;)
    {
        asm volatile("sti");
        asm volatile("hlt");
    }
}

static void spin_wait()
{
    for (volatile int i = 0; i < 200000; ++i) {}
}

static void task_a(void*)
{
    while (true)
    {
        qcos::Serial::putc('A');
        spin_wait();
    }
}

static void task_b(void*)
{
    while (true)
    {
        qcos::Serial::putc('B');
        spin_wait();
    }
}

extern "C" void kernel_main()
{
    using namespace qcos;

    Serial::init();
    Serial::puts("QCOS: hello from kernel!\n");

    for (int i = 0; i < 256; ++i)
        g_idt.set_gate(static_cast<u8>(i), isr_table[i], GateType::INTERRUPT, 0);

    pic_remap();
    pic_set_mask(0xFFFE);
    pit_init(1000);
    g_idt.load();

    Serial::puts("QCOS: idt+pic+pit ready\n");

    thread_init(&g_thread_a, task_a, nullptr, g_stack_a + sizeof(g_stack_a));
    thread_init(&g_thread_b, task_b, nullptr, g_stack_b + sizeof(g_stack_b));
    g_thread_a.id = 1; g_thread_a.weight = 100; g_thread_a.slice = 5;
    g_thread_b.id = 2; g_thread_b.weight = 100; g_thread_b.slice = 5;
    g_sched.add(&g_thread_a);
    g_sched.add(&g_thread_b);

    thread_init(&g_thread_idle, task_idle, nullptr, g_stack_idle + sizeof(g_stack_idle));
    g_thread_idle.id = 0; g_thread_idle.weight = 1; g_thread_idle.slice = 1;
    g_sched.add(&g_thread_idle);

    g_sched.tick();
    Thread* first = g_sched.current();
    qcos_start_scheduler(first->kernel_rsp);

    halt();
}