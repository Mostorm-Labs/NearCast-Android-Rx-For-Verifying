# NearCast Android RX Demo

Android 原生界面与单路 GStreamer 播放。App 在前台发布当前接收模式，切换协议时关闭旧会话并注销旧发现记录。设备名为 `NearCast-Android-XXXX`，同一安装内保持一致。

| 模式 | 发现与连接 | 媒体 |
| --- | --- | --- |
| AirPlay | Android NSD 发布 `_airplay._tcp` 和 `_raop._tcp`，复用 Windows RX 的 UxPlay 认证和镜像会话，按需显示 PIN | 镜像视频和协商音频进入 GStreamer |
| WebCast | `_nearcast._tcp`，匹配 NearCast Desktop TX 的 LAN 发现、SPKI 指纹确认与四位投屏码流程，HTTPS/WSS 承载信令 | 复用 RX 的 `webrtcbin` 接收，支持 H.264 与 Opus |

## 构建

需要 JDK 17 或更新版本、Android SDK 35、NDK `27.2.12479018`、Git、Bash 和 C++ 编译器。APK 面向 Android 7.0 及以上的 ARM64 设备。

默认从同级 `NearCast` 仓库读取 RX 提交 `c6e12701c9ac6069a263084468db703e66118dfb` 和相应子模块对象。其他路径可以设置 `NEARCAST_RX_ROOT`。脚本从 Git 对象提取源码，不需要在 RX 仓库切换分支。GStreamer 使用官方 Android 1.28.4 SDK，脚本校验下载的 SHA-256，只解压 ARM64 前缀。已有 SDK 可通过 `GSTREAMER_ROOT_ANDROID` 指定，目录内应含 `arm64/`。 [GStreamer Android 1.28.4](https://gstreamer.freedesktop.org/pkg/android/1.28.4/)

```sh
./scripts/prepare-native.sh
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

`./scripts/test-host.sh` 验证 WebCast 本地信令、投屏码轮换、DataChannel 回退和 UxPlay TXT 交接。Android 播放层使用 `SurfaceView`、GStreamer `appsrc`、`glimagesink` 与 `openslessink`。WebCast SDK 还需要 libnice、RTP、DTLS/SRTP 和 SCTP。AirPlay 协议核心依赖 OpenSSL 和 libplist。

## 联调

1. 将 Android 接收端和发送端连到同一允许设备互访的局域网，保持 App 在前台。Wi-Fi 和以太网均需具备可用的 IPv4 地址。
2. 选 **AirPlay**，从 iPhone 或 Mac 的「屏幕镜像」选择设备名，按提示输入 Android 显示的密码。验证首帧、声音、断开和再次投屏。
3. 选 **WebCast**，在 NearCast Desktop TX 保持「局域网投屏」开启，从设备列表选择 Android。首次核对两端显示的完整指纹，确认后输入四位投屏码并选择共享源。

## 已验证

- ARM64 Android 14 模拟器完成两种模式的 NSD 注册和切换。AirPlay 用合成发送端验证了 PIN、加密 H.264 出画和断开清屏。
- WebCast 用 Desktop TX 的发送端代码发送测试画面和音频，验证了 HTTPS 配对、WebRTC 出画、48 kHz 播放轨道、断开清屏和退到后台注销。

## 真机验收待确认

| 项目 | 需要确认 |
| --- | --- |
| Android 接收设备 | 具体盒子、电视或手机型号，系统版本、ABI、Wi-Fi/以太网环境和多播可达性 |
| AirPlay 发送端 | 首轮 iPhone/Mac 型号与版本，是否将系统音频纳入验收 |
| WebCast 发送端 | 桌面端 LAN 为本 Demo 入口。现有浏览器或 Android TX 的云端路径如需纳入，确定客户端版本与信令环境 |
| 媒体验收 | 首帧耗时、清晰度、音画同步和断开重投的验收指标 |
| NT10 RTSP | 推流或拉流、URL / SDP 样例、鉴权、TCP/UDP 传输与音视频编码 |

构建输入保留 NearCast、UxPlay、GStreamer、libplist、IXWebSocket 和 nlohmann/json 的许可证与来源。对外分发前确认使用方需要的第三方许可材料。
