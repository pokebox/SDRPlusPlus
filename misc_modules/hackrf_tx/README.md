# hackrf_tx — HackRF One 录制 / 回放模块

给 SDR++ 增加 HackRF One 的发射能力：把瀑布图上选定的一段频谱录下来，再原样发回去。

SDR++ 原生只有接收链路，本模块自带一条独立的 IQ 录制/回放路径，不改动原有 DSP 图。

## 功能

- **录制**：按指定采样率创建一个专用 VFO（瀑布上可见可拖动），把该 VFO 的复基带以 WAV 写入磁盘，同时记录中心频率与采样率。
- **回放**：把录制文件整体载入内存并转换为 HackRF 原生 int8 格式，通过 `libhackrf` 的 TX 回调实时供数发射。
- **半双工切换**：HackRF 不能同时收发。发射前自动停止接收，发射结束（或中止）后自动恢复，接收按钮状态与界面保持一致。

## 使用

1. 左侧菜单打开 **HackRF TX** 面板。
2. **Record** 区选采样率（2/4/5/8/10/16/20 MHz）与位深（Int16 / Float32）。
   - 采样率应不高于接收源当前采样率，超出部分会由上采样补齐，信号带宽不会变宽。
   - Int16 体积是 Float32 的一半，两者都能被 SDR++ 自带的 File Source 播放。
3. 点 **Record** 开始录制。此时瀑布上会出现一个带宽等于采样率的选框，**录制期间不要移动它**，中心频率在开始录制时锁定。
4. 点 **Stop recording** 结束。文件写入 `<root>/recordings/rec_<时间戳>.wav`，同名 `.json` 保存中心频率、采样率、样本数。
5. 在 **Recording file** 下拉里选择录音，**Frequency** 会自动填成录制时的中心频率（可手动改）。
6. 设置 **TX VGA gain**（0–47 dB）与 **Amp enabled**，点 **Transmit**。

## 文件格式

- WAV，2 声道（I/Q 交织），16-bit PCM 或 32-bit float。
- 采样率等于录制时选择的 VFO 采样率，回放时 1:1 使用，不做重采样。
- 元数据 `.json`：`frequency`（Hz）、`sampleRate`、`samples`、`duration`、`timestamp`。

## 构建

模块随主工程一起构建，依赖系统 `libhackrf`：

```cmake
pkg_check_modules(LIBHACKRF REQUIRED libhackrf)
```

关闭方式：`-DOPT_BUILD_HACKRF_TX=OFF`。

## 已知限制

- 回放缓冲整体放在内存里，单次上限 1 GB（int8，约 2 字节/复样本）。
- 录制为直写磁盘，20 MHz / Float32 时约 160 MB/s，建议放在 SSD 上；日常使用 2 MHz / Int16 约 8 MB/s。
- 录制期间若拖动频谱选框，文件内容会跟着变，但元数据仍是开始时的中心频率。
- HackRF 发射存在本振泄漏与 IQ 不平衡，频谱中心会有杂散，属于硬件特性。

## 合规

发射前请确认所用频点在当地法规允许范围内，或使用假负载/屏蔽箱测试。
