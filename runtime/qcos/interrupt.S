# ============================================================================
#  x86_64 中断入口 stub 与调度启动
#
#  中断发生时 CPU push rip/cs/rflags，为 256 个向量提供 stub，统一进入
#  isr_common：保存 15 个通用寄存器 → call qcos_interrupt_dispatch(frame) →
#  用返回值（新任务栈指针）切换 rsp → 恢复寄存器 → iretq。
#
#  任务上下文 = 任务内核栈上保存的 InterruptFrame（见 include/qcos/Idt.hpp），
#  因此抢占式切换 = 换栈指针 + iretq，天然无锁、无额外上下文拷贝。
# ============================================================================

.section .text

# ---- 中断 stub 宏（.altmacro：%n 展开参数）----
.altmacro

.macro ISR_NOERR n
.global isr_\n
.type isr_\n, @function
isr_\n:
    pushq $0            # 伪错误码（统一帧布局）
    pushq $\n           # 向量号
    jmp isr_common
.endm

.macro ISR_ERR n
.global isr_\n
.type isr_\n, @function
isr_\n:
    pushq $\n           # 向量号（错误码已由 CPU push）
    jmp isr_common
.endm

# ---- CPU 异常 0-31 ----
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR 8
ISR_NOERR 9
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR 30
ISR_NOERR 31

# ---- IRQ 32-47 + 保留 48-255 ----
.set i, 32
.rept 224
    ISR_NOERR %i
    .set i, i+1
.endr

# ---- 通用中断入口 ----
#   入栈顺序（自高到低）：rip, cs, rflags(CPU) → err_code, vector(stub)
#   → rax, rbx, rcx, rdx, rsi, rdi, rbp, r8..r15（本段）。rsp 最终指向 r15。
isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi          # rdi = 指向 InterruptFrame
    callq qcos_interrupt_dispatch
    movq %rax, %rsp          # rax = 新任务栈指针（切换上下文）
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp           # 移除 vector + err_code
    iretq

# ---- 启动调度器：进入第一个任务（noreturn）----
#   void qcos_start_scheduler(u64 first_rsp)；rdi = 第一个任务的栈指针
.global qcos_start_scheduler
.type qcos_start_scheduler, @function
qcos_start_scheduler:
    movq %rdi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    iretq

# ---- 中断处理函数地址表（供 C++ 设置 IDT）----
.section .data
.global isr_table
.align 8
isr_table:
    .quad isr_0
    .quad isr_1
    .quad isr_2
    .quad isr_3
    .quad isr_4
    .quad isr_5
    .quad isr_6
    .quad isr_7
    .quad isr_8
    .quad isr_9
    .quad isr_10
    .quad isr_11
    .quad isr_12
    .quad isr_13
    .quad isr_14
    .quad isr_15
    .quad isr_16
    .quad isr_17
    .quad isr_18
    .quad isr_19
    .quad isr_20
    .quad isr_21
    .quad isr_22
    .quad isr_23
    .quad isr_24
    .quad isr_25
    .quad isr_26
    .quad isr_27
    .quad isr_28
    .quad isr_29
    .quad isr_30
    .quad isr_31
    .quad isr_32
    .quad isr_33
    .quad isr_34
    .quad isr_35
    .quad isr_36
    .quad isr_37
    .quad isr_38
    .quad isr_39
    .quad isr_40
    .quad isr_41
    .quad isr_42
    .quad isr_43
    .quad isr_44
    .quad isr_45
    .quad isr_46
    .quad isr_47
    .quad isr_48
    .quad isr_49
    .quad isr_50
    .quad isr_51
    .quad isr_52
    .quad isr_53
    .quad isr_54
    .quad isr_55
    .quad isr_56
    .quad isr_57
    .quad isr_58
    .quad isr_59
    .quad isr_60
    .quad isr_61
    .quad isr_62
    .quad isr_63
    .quad isr_64
    .quad isr_65
    .quad isr_66
    .quad isr_67
    .quad isr_68
    .quad isr_69
    .quad isr_70
    .quad isr_71
    .quad isr_72
    .quad isr_73
    .quad isr_74
    .quad isr_75
    .quad isr_76
    .quad isr_77
    .quad isr_78
    .quad isr_79
    .quad isr_80
    .quad isr_81
    .quad isr_82
    .quad isr_83
    .quad isr_84
    .quad isr_85
    .quad isr_86
    .quad isr_87
    .quad isr_88
    .quad isr_89
    .quad isr_90
    .quad isr_91
    .quad isr_92
    .quad isr_93
    .quad isr_94
    .quad isr_95
    .quad isr_96
    .quad isr_97
    .quad isr_98
    .quad isr_99
    .quad isr_100
    .quad isr_101
    .quad isr_102
    .quad isr_103
    .quad isr_104
    .quad isr_105
    .quad isr_106
    .quad isr_107
    .quad isr_108
    .quad isr_109
    .quad isr_110
    .quad isr_111
    .quad isr_112
    .quad isr_113
    .quad isr_114
    .quad isr_115
    .quad isr_116
    .quad isr_117
    .quad isr_118
    .quad isr_119
    .quad isr_120
    .quad isr_121
    .quad isr_122
    .quad isr_123
    .quad isr_124
    .quad isr_125
    .quad isr_126
    .quad isr_127
    .quad isr_128
    .quad isr_129
    .quad isr_130
    .quad isr_131
    .quad isr_132
    .quad isr_133
    .quad isr_134
    .quad isr_135
    .quad isr_136
    .quad isr_137
    .quad isr_138
    .quad isr_139
    .quad isr_140
    .quad isr_141
    .quad isr_142
    .quad isr_143
    .quad isr_144
    .quad isr_145
    .quad isr_146
    .quad isr_147
    .quad isr_148
    .quad isr_149
    .quad isr_150
    .quad isr_151
    .quad isr_152
    .quad isr_153
    .quad isr_154
    .quad isr_155
    .quad isr_156
    .quad isr_157
    .quad isr_158
    .quad isr_159
    .quad isr_160
    .quad isr_161
    .quad isr_162
    .quad isr_163
    .quad isr_164
    .quad isr_165
    .quad isr_166
    .quad isr_167
    .quad isr_168
    .quad isr_169
    .quad isr_170
    .quad isr_171
    .quad isr_172
    .quad isr_173
    .quad isr_174
    .quad isr_175
    .quad isr_176
    .quad isr_177
    .quad isr_178
    .quad isr_179
    .quad isr_180
    .quad isr_181
    .quad isr_182
    .quad isr_183
    .quad isr_184
    .quad isr_185
    .quad isr_186
    .quad isr_187
    .quad isr_188
    .quad isr_189
    .quad isr_190
    .quad isr_191
    .quad isr_192
    .quad isr_193
    .quad isr_194
    .quad isr_195
    .quad isr_196
    .quad isr_197
    .quad isr_198
    .quad isr_199
    .quad isr_200
    .quad isr_201
    .quad isr_202
    .quad isr_203
    .quad isr_204
    .quad isr_205
    .quad isr_206
    .quad isr_207
    .quad isr_208
    .quad isr_209
    .quad isr_210
    .quad isr_211
    .quad isr_212
    .quad isr_213
    .quad isr_214
    .quad isr_215
    .quad isr_216
    .quad isr_217
    .quad isr_218
    .quad isr_219
    .quad isr_220
    .quad isr_221
    .quad isr_222
    .quad isr_223
    .quad isr_224
    .quad isr_225
    .quad isr_226
    .quad isr_227
    .quad isr_228
    .quad isr_229
    .quad isr_230
    .quad isr_231
    .quad isr_232
    .quad isr_233
    .quad isr_234
    .quad isr_235
    .quad isr_236
    .quad isr_237
    .quad isr_238
    .quad isr_239
    .quad isr_240
    .quad isr_241
    .quad isr_242
    .quad isr_243
    .quad isr_244
    .quad isr_245
    .quad isr_246
    .quad isr_247
    .quad isr_248
    .quad isr_249
    .quad isr_250
    .quad isr_251
    .quad isr_252
    .quad isr_253
    .quad isr_254
    .quad isr_255