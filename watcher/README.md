# SenseCAP Watcher 原生 IMPO 固件

此分支保存已在实物接入 IMPO 的 **0.1.1** 完整固件源码及 Watcher 构建配置。它直接基于官方固定提交建立，保留完整上游源码和历史；`watcher/` 是本次验收配置的构建入口。目标为 ESP32-S3、32 MiB Flash、8 MiB PSRAM 的 Seeed SenseCAP Watcher，ESP-IDF **6.0.1**。

## 已验收的行为

本人手机完成 BLE 配对后，设备用 Wi-Fi 直接连接 IMPO，不依赖电脑常驻中转。2026-11-27 实测通过中文显示、清晰语音播报，以及按住滚轮说话后松开的完整语音问答。一次 4.06 秒录音的首次回复音频在松开后约 11.4 秒开始，回复音频共 21 秒；这是一轮功能试验，不代表长期稳定性或延迟分布。目标构建通过；主机测试 261 项，260 通过、1 跳过。详见 [ACCEPTANCE.json](ACCEPTANCE.json)。

摄像头在原小智固件报传感器配置失败，原生 IMPO 也未取得 Himax 响应，因此当前明确关闭拍照/视觉能力。触摸、图片/音频直链、续航、断开电脑后的独立供电、原固件完整恢复和长期稳定性尚未验收；启动有触摸 I2C 读失败记录。没有改写 Himax 固件。设备无电机；SD/Grove 和环境传感器没有适配。

## 源码和改动

[SOURCES.json](SOURCES.json) 固定官方 `impoai/impo-gadget-sdk` 提交 `90d1fbd5939f00eadd3f867d9cf7c15f779a7065`、实际修改文件摘要及组件依赖锁。完整 SDK 在本仓库根目录，源码可直接浏览，板级引脚沿用官方实现。

本次修改：[板级驱动](../esp32/components/impo/boards/board_sensecap_watcher.c)、[语音任务](../esp32/components/impo/impo_voice.c)、[Kconfig](../esp32/components/impo/Kconfig)、[组件构建](../esp32/components/impo/CMakeLists.txt)、[嵌入设备描述](../esp32/components/impo/watcher_description.json)。统一增量在 [runtime.patch](runtime.patch)。主要改动是嵌入 Watcher 的真实能力描述，以及可选关闭旧录音自动重发；该配置启用此选项，避免网络恢复后旧输入再次占用语音任务。[sdkconfig.watcher](sdkconfig.watcher) 是应用配置，[dependencies.lock](dependencies.lock) 固定实机构建所用的组件版本。

当前固件自描述 9 条设备命令，加平台 `device.health` 共 10 条；网关 `speaker.say` 准备音频后调用本机 `speaker.play`。不使用旧固件的 `speaker.beep` / `voice.configure`。AAC-LC 语音输入及官方中国大陆 TLS/Noise 入口来自固定版上游。原小智唤醒词不保留，使用滚轮按住说话。

## 从干净目录还原、构建

安装并启用官方 ESP-IDF **6.0.1**，Python **3.10+**，以及主机测试需要的 C/C++ 编译器和 CMake。克隆本分支并进入本目录后运行：

```sh
git clone --branch codex/watcher-impo-0.1.1-20261127 https://github.com/LeyiGitHub/impo-gadget-sdk.git
cd impo-gadget-sdk/watcher
. /path/to/esp-idf-v6.0.1/export.sh
python3 build.py build
python3 build.py test
```

源码无需另行解包，GitHub 的分支源码 ZIP 也包含全部固件文件。构建检查实际修改文件和依赖锁的 SHA256，并在 `esp32/build-impo-sensecap-watcher/` 生成应用；依赖锁不符时拒绝混用其他板型的组件状态，优先使用干净 checkout。仍需安装工具链，首次构建还会下载固定版本组件。需要 SDK token 的配对环境应由使用者在本机私有构建配置中提供，不提交 token。

应用输出为 `esp32/build-impo-sensecap-watcher/impo-gadget.bin`。`build.py verify --build /path/to/build` 检查板型、32 MiB Flash、PSRAM、分区、中文字体、录音重发配置、镜像余量及设备描述。构建不会访问或刷写硬件；生成镜像不表示实物验收完成。SOURCES 记录的是原设备已验收镜像的摘要，其他构建路径或生成元数据可能改变重建镜像摘要，不能据此冒称二进制一致。

## 备份、配对与升级边界

原设备迁移前已保存两份经过逐块和独立全片设备 MD5 核验的 **32 MiB** 原固件备份；备份含配置与凭据，留在本机。已配对的 0.1.0 → 0.1.1 升级只写应用区 `0x20000`，并确认 NVS、otadata、product-data 的设备摘要未变化。原固件完整恢复尚未演练。

任何后续写入前，重新核对实物 MAC、Flash 容量、当前分区和已验证备份。CH342 有两个通道，已验实物尾号 `3` 是 ESP32，另一通道连接 Himax；不要把另一个端口当作 ESP32。Watcher 写入必须使用源码中的 `esp32/tools/impo/paced_esptool.py`，不能使用普通高速写入。不要对已配对设备运行首次迁移/擦除流程，也不要仅凭应用偏移相同假定原小智布局与 IMPO 布局兼容。

本目录不包含设备 Flash 备份、NVS、配对凭据、开发者密钥、账户标识或原始串口日志。许可证保留 [LICENSE](../LICENSE)、[LICENSE-APACHE](../LICENSE-APACHE)、[NOTICE](../NOTICE) 与各文件原有版权；没有重新统一许可第三方源码。
