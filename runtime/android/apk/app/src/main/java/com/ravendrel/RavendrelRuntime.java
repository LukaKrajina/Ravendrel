package com.ravendrel;

/**
 * Ravendrel 运行时 JNI 壳：加载 librvdl_rt.so（量子核心）+ librvdl_main.so（AOT 编译的 rvdl 代码）。
 */
public class RavendrelRuntime {
    static {
        System.loadLibrary("rvdl_rt");
        System.loadLibrary("rvdl_main");
    }

    /** 初始化量子后端（创建 QVM）。 */
    public native void nativeInit();

    /** 释放量子后端。 */
    public native void nativeShutdown();

    /** 自检：创建一个 Bell 态并测量，返回测量结果（0 或 1）。 */
    public native int nativeBellMeasure();

    /** 执行 AOT 编译的 rvdl_main 函数，返回其 int 返回值。 */
    public native int nativeRavendrelMain();
}