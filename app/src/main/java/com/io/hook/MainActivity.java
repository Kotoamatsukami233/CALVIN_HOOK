package com.io.hook;

import androidx.appcompat.app.AppCompatActivity;

import android.os.Bundle;
import android.util.Log;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ScrollView;
import android.widget.TextView;

import com.io.hook.databinding.ActivityMainBinding;

public class MainActivity extends AppCompatActivity {

    private static final String TAG = "HookDemo";

    static {
        System.loadLibrary("hook");
    }

    private ActivityMainBinding binding;
    private TextView tvOutput;
    private ScrollView scrollOutput;
    private EditText etScript;

    private static final String DEFAULT_SCRIPT =
            "var openPtr = Module.findExportByName(\"libc.so\", \"open\");\n" +
            "if (openPtr) {\n" +
            "    Interceptor.attach(openPtr, {\n" +
            "        onEnter: function(ctx) {\n" +
            "            var path = Memory.readUtf8String(ctx.x0);\n" +
            "            var flags = ctx.x1.toInt32();\n" +
            "            send(\"open(\\\"\" + path + \"\\\", \" + flags + \")\");\n" +
            "        },\n" +
            "        onLeave: function(ctx) {\n" +
            "            send(\"  -> fd = \" + ctx.retval.toInt32());\n" +
            "        }\n" +
            "    });\n" +
            "    send(\"Hook installed on open @ \" + openPtr);\n" +
            "} else {\n" +
            "    send(\"ERROR: Cannot find open in libc.so\");\n" +
            "}";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        etScript = binding.etScript;
        etScript.setText(DEFAULT_SCRIPT);

        tvOutput = binding.tvOutput;
        scrollOutput = binding.scrollOutput;

        // Route JS send() output to the TextView
        ScriptEngine.setMessageListener(msg -> runOnUiThread(() -> appendOutput(msg)));

        Button btnRunScript = binding.btnRunScript;
        Button btnTestOpen = binding.btnTestOpen;

        btnRunScript.setOnClickListener(v -> {
            String script = etScript.getText().toString();
            appendOutput(">>> Running script...");
            String result = ScriptEngine.loadScript(script);
            appendOutput("Result: " + result);
        });

        // Auto-inject on startup for debugging
        new Thread(() -> {
            try { Thread.sleep(500); } catch (InterruptedException ignored) {}
            String script = etScript.getText().toString();
            Log.i(TAG, "Auto-injecting script...");
            String result = ScriptEngine.loadScript(script);
            Log.i(TAG, "Auto-inject result: " + result);
        }).start();

        btnTestOpen.setOnClickListener(v -> {
            appendOutput(">>> Calling open(\"/proc/self/maps\")...");
            String result = nativeTestOpen();
            appendOutput("Result: " + result);
        });
    }

    private void appendOutput(String line) {
        tvOutput.append(line + "\n");
        scrollOutput.post(() -> scrollOutput.fullScroll(ScrollView.FOCUS_DOWN));
    }

    public native String nativeTestOpen();
}
