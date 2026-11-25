package com.mostorm.nearcast.rx;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.freedesktop.gstreamer.GStreamer;

import java.net.Inet4Address;
import java.util.HashSet;
import java.util.Locale;
import java.util.Map;
import java.util.HashMap;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class ReceiverActivity extends Activity implements SurfaceHolder.Callback {
    static {
        System.loadLibrary("gstreamer_android");
        System.loadLibrary("nearcast_rx");
    }

    private static final ExecutorService RECEIVER = Executors.newSingleThreadExecutor();
    private static boolean gstInitialized;
    private long nativeHandle;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Map<String, NsdManager.RegistrationListener> registrations = new HashMap<>();
    private final Set<String> registered = new HashSet<>();

    private volatile long generation;
    private NsdManager nsd;
    private Surface surface;
    private Network network;
    private String deviceId;
    private String deviceName;
    private String webCode = "";
    private String pinId = "";
    private boolean foreground;
    private boolean active;
    private boolean web;
    private TextView status;
    private TextView pairing;
    private TextView fingerprint;
    private View idleCover;
    private Button toggle;
    private Button airPlayButton;
    private Button webCastButton;
    private Button disconnect;
    private final Runnable hideFingerprint = () -> {
        fingerprint.setVisibility(View.GONE);
        fingerprint.setText("");
    };

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        nsd = getSystemService(NsdManager.class);
        SharedPreferences preferences = getSharedPreferences("receiver", MODE_PRIVATE);
        deviceId = preferences.getString("id", "");
        if (deviceId.length() != 32) {
            deviceId = UUID.randomUUID().toString().replace("-", "").toUpperCase(Locale.ROOT);
            preferences.edit().putString("id", deviceId).apply();
        }
        deviceName = "NearCast-Android-" + deviceId.substring(0, 4);
        web = preferences.getBoolean("webcast", false);
        buildUi();
    }

    private void buildUi() {
        FrameLayout layout = new FrameLayout(this);
        layout.setBackgroundColor(Color.BLACK);
        layout.setFitsSystemWindows(true);
        SurfaceView video = new SurfaceView(this);
        video.getHolder().addCallback(this);
        layout.addView(video, new FrameLayout.LayoutParams(-1, -1));
        idleCover = new View(this);
        idleCover.setBackgroundColor(Color.BLACK);
        layout.addView(idleCover, new FrameLayout.LayoutParams(-1, -1));

        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(16), dp(10), dp(16), dp(10));
        panel.setBackgroundColor(0xe618202a);
        FrameLayout.LayoutParams panelParams = new FrameLayout.LayoutParams(-1, -2, Gravity.TOP);
        layout.addView(panel, panelParams);

        TextView name = label(deviceName, 19);
        name.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        panel.addView(name);
        LinearLayout modes = new LinearLayout(this);
        panel.addView(modes, new LinearLayout.LayoutParams(-1, -2));
        airPlayButton = addButton(modes, "AirPlay", () -> select(false));
        webCastButton = addButton(modes, "WebCast", () -> select(true));
        toggle = addButton(modes, "启动接收", () -> {
            if (active) stop("接收已停止");
            else start();
        });

        status = label("等待网络", 15);
        panel.addView(status);
        pairing = label("", 23);
        pairing.setTypeface(Typeface.MONOSPACE, Typeface.NORMAL);
        pairing.setVisibility(View.GONE);
        panel.addView(pairing);

        fingerprint = label("", 16);
        fingerprint.setTypeface(Typeface.MONOSPACE, Typeface.NORMAL);
        fingerprint.setVisibility(View.GONE);
        panel.addView(fingerprint);
        disconnect = new Button(this);
        disconnect.setText("断开当前投屏");
        disconnect.setAllCaps(false);
        disconnect.setVisibility(View.GONE);
        disconnect.setOnClickListener(view -> RECEIVER.execute(() -> nativeDisconnect(nativeHandle)));
        panel.addView(disconnect, new LinearLayout.LayoutParams(-2, -2));
        setContentView(layout);
        updateButtons();
    }

    private TextView label(String text, int size) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextColor(0xfff3f7fb);
        view.setTextSize(size);
        return view;
    }

    private Button addButton(LinearLayout parent, String text, Runnable click) {
        Button button = new Button(this);
        button.setText(text);
        button.setAllCaps(false);
        button.setOnClickListener(view -> click.run());
        parent.addView(button, new LinearLayout.LayoutParams(0, -2, 1));
        return button;
    }

    private int dp(int size) {
        return Math.round(size * getResources().getDisplayMetrics().density);
    }

    private void select(boolean useWebCast) {
        if (web == useWebCast) return;
        stop("正在切换接收模式");
        web = useWebCast;
        getSharedPreferences("receiver", MODE_PRIVATE).edit().putBoolean("webcast", web).apply();
        start();
        updateButtons();
    }

    private void updateButtons() {
        airPlayButton.setEnabled(web);
        webCastButton.setEnabled(!web);
        toggle.setText(active ? "停止接收" : "启动接收");
    }

    @Override protected void onStart() {
        super.onStart();
        foreground = true;
        start();
    }

    @Override protected void onStop() {
        foreground = false;
        stop("接收已停止");
        super.onStop();
    }

    @Override public void surfaceCreated(SurfaceHolder holder) {
        surface = holder.getSurface();
        start();
    }

    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        surface = null;
        stop("接收已停止");
    }

    private String findLanAddress() {
        ConnectivityManager connectivity = getSystemService(ConnectivityManager.class);
        for (Network candidate : connectivity.getAllNetworks()) {
            NetworkCapabilities caps = connectivity.getNetworkCapabilities(candidate);
            if (caps == null || (!caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) &&
                    !caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET))) continue;
            LinkProperties properties = connectivity.getLinkProperties(candidate);
            if (properties == null) continue;
            for (LinkAddress link : properties.getLinkAddresses()) {
                if (link.getAddress() instanceof Inet4Address && !link.getAddress().isLoopbackAddress()) {
                    network = candidate;
                    return link.getAddress().getHostAddress();
                }
            }
        }
        network = null;
        return null;
    }

    private void start() {
        if (!foreground || surface == null || active) return;
        String host = findLanAddress();
        if (host == null) {
            status.setText("请连接有 IPv4 地址的 Wi-Fi 或以太网");
            return;
        }
        long token = ++generation;
        Surface target = surface;
        boolean mode = web;
        String directory = getFilesDir().getAbsolutePath();
        StringBuilder mac = new StringBuilder("02");
        for (int i = 0; i < 10; i += 2) mac.append(':').append(deviceId, i, i + 2);
        active = true;
        updateButtons();
        status.setText("正在启动接收和局域网发现");
        RECEIVER.execute(() -> {
            if (generation != token || !target.isValid()) return;
            try {
                if (!gstInitialized) {
                    GStreamer.init(getApplicationContext());
                    gstInitialized = true;
                }
                nativeHandle = nativeStart(target, token, mode, directory, deviceName, deviceId, mac.toString(), host);
                if (nativeHandle == 0) onNativeEvent(token, "startupError", "接收服务启动失败");
            } catch (Exception failure) {
                onNativeEvent(token, "startupError", "GStreamer 初始化失败");
            }
        });
    }

    private void stop(String text) {
        if (status == null) return;
        generation++;
        active = false;
        for (NsdManager.RegistrationListener listener : registrations.values()) unregister(listener);
        registrations.clear();
        registered.clear();
        ui.removeCallbacks(hideFingerprint);
        hideFingerprint.run();
        webCode = "";
        pinId = "";
        updatePairing("");
        idleCover.setVisibility(View.VISIBLE);
        disconnect.setVisibility(View.GONE);
        status.setText(text);
        updateButtons();
        RECEIVER.execute(() -> {
            if (nativeHandle != 0) {
                nativeStop(nativeHandle);
                nativeHandle = 0;
            }
        });
    }

    private void unregister(NsdManager.RegistrationListener listener) {
        try { nsd.unregisterService(listener); } catch (IllegalArgumentException ignored) {}
    }

    private void onNativeService(long token, String type, String name, int port, String[] txt) {
        runOnUiThread(() -> {
            if (token != generation || !active) return;
            NsdServiceInfo service = new NsdServiceInfo();
            service.setServiceType(type);
            service.setServiceName(name);
            service.setPort(port);
            if (Build.VERSION.SDK_INT >= 33) service.setNetwork(network);
            for (String item : txt) {
                int separator = item.indexOf('=');
                if (separator > 0) service.setAttribute(item.substring(0, separator), item.substring(separator + 1));
            }
            NsdManager.RegistrationListener listener = new NsdManager.RegistrationListener() {
                @Override public void onServiceRegistered(NsdServiceInfo info) {
                    runOnUiThread(() -> {
                        if (generation != token) {
                            unregister(this);
                            return;
                        }
                        if (!web && !info.getServiceName().equals(name)) {
                            stop("AirPlay 接收器名称冲突");
                            return;
                        }
                        registered.add(type);
                        if (registered.size() == (web ? 1 : 2)) showWaiting();
                    });
                }
                @Override public void onRegistrationFailed(NsdServiceInfo info, int error) {
                    runOnUiThread(() -> {
                        if (generation == token) stop("局域网发现注册失败 (" + error + ")");
                    });
                }
                @Override public void onServiceUnregistered(NsdServiceInfo info) {}
                @Override public void onUnregistrationFailed(NsdServiceInfo info, int error) {}
            };
            registrations.put(type, listener);
            try {
                nsd.registerService(service, NsdManager.PROTOCOL_DNS_SD, listener);
            } catch (RuntimeException failure) {
                stop("无法发布局域网发现记录");
            }
        });
    }

    private void onNativeEvent(long token, String type, String value) {
        runOnUiThread(() -> {
            if (generation != token || !active) return;
            switch (type) {
                case "code":
                    webCode = value;
                    updatePairing("WebCast 投屏码  " + webCode);
                    break;
                case "showCode":
                    ui.removeCallbacks(hideFingerprint);
                    hideFingerprint.run();
                    updatePairing("WebCast 投屏码  " + webCode);
                    break;
                case "fingerprint":
                    showFingerprint(value);
                    break;
                case "pin":
                    int separator = value.indexOf(':');
                    pinId = value.substring(0, separator);
                    updatePairing("AirPlay 投屏密码  " + value.substring(separator + 1));
                    disconnect.setVisibility(View.VISIBLE);
                    break;
                case "pinDismissed":
                    if (value.equals(pinId)) {
                        pinId = "";
                        updatePairing("");
                    }
                    break;
                case "connecting":
                    idleCover.setVisibility(View.VISIBLE);
                    status.setText("已配对，等待发送端开始共享");
                    disconnect.setVisibility(View.VISIBLE);
                    break;
                case "media":
                    if ("video".equals(value)) {
                        idleCover.setVisibility(View.GONE);
                        status.setText("正在投屏");
                    }
                    disconnect.setVisibility(View.VISIBLE);
                    break;
                case "waiting":
                    idleCover.setVisibility(View.VISIBLE);
                    showWaiting();
                    disconnect.setVisibility(View.GONE);
                    break;
                case "startupError":
                    stop(value);
                    break;
                case "error":
                    idleCover.setVisibility(View.VISIBLE);
                    status.setText(value);
                    break;
                default:
                    break;
            }
        });
    }

    private void showWaiting() {
        status.setText(web ? "等待投屏 · 在 NearCast Desktop TX 的局域网设备列表中选择此设备" :
                "等待投屏 · 在 iPhone 或 Mac 的屏幕镜像中选择此设备");
    }

    private void updatePairing(String text) {
        pairing.setText(text);
        pairing.setVisibility(text.isEmpty() ? View.GONE : View.VISIBLE);
    }

    private void showFingerprint(String value) {
        if (value.length() != 64) return;
        StringBuilder text = new StringBuilder("在发送端核对设备指纹\n");
        for (int i = 0; i < value.length(); i += 4) {
            if (i > 0) text.append(i % 16 == 0 ? '\n' : ' ');
            text.append(value, i, i + 4);
        }
        fingerprint.setText(text);
        fingerprint.setVisibility(View.VISIBLE);
        ui.removeCallbacks(hideFingerprint);
        ui.postDelayed(hideFingerprint, 60_000);
    }

    private native long nativeStart(Surface target, long token, boolean useWebCast,
            String directory, String name, String id, String airPlayId, String host);
    private native void nativeStop(long handle);
    private native void nativeDisconnect(long handle);
}
