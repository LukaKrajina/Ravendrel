package com.ravendrel;

import android.app.Activity;
import android.os.Bundle;
import android.graphics.Color;
import android.widget.TextView;

/**
 * 极简 Activity：初始化 Ravendrel 运行时，执行 AOT 编译的 rvdl_main，显示结果。
 */
public class MainActivity extends Activity {
    private RavendrelRuntime runtime;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        TextView tv = new TextView(this);
        tv.setTextColor(Color.WHITE);
        tv.setTextSize(18);
        tv.setPadding(48, 48, 48, 48);
        setContentView(tv);

        try {
            runtime = new RavendrelRuntime();
            runtime.nativeInit();

            int bellResult = runtime.nativeBellMeasure();
            int ravendrelResult = runtime.nativeRavendrelMain();

            StringBuilder sb = new StringBuilder();
            sb.append("Ravendrel Runtime on Android\n\n");
            sb.append("Bell measure (self-test): ").append(bellResult).append("\n");
            sb.append("rvdl_main() result: ").append(ravendrelResult).append("\n");
            tv.setText(sb.toString());
        } catch (Throwable t) {
            tv.setText("Ravendrel error:\n" + t.toString());
        }
    }

    @Override
    protected void onDestroy() {
        if (runtime != null) {
            runtime.nativeShutdown();
            runtime = null;
        }
        super.onDestroy();
    }
}