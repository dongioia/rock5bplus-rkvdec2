---
title: NPU
description: Setting up and using the Neural Processing Unit on Rockchip SoCs with BredOS
published: false
date: 2026-03-10T12:00:00.000Z
tags: npu, rk3588, ai, machine-learning
editor: markdown
dateCreated: 2026-02-18T09:54:15.539Z
---

# 1. Introduction

Some Rockchip SoCs include a dedicated Neural Processing Unit (`NPU`) designed to accelerate machine learning inference. The `RK3588` integrates a 6 TOPS NPU with 3 cores, capable of running quantized neural network models significantly faster than the CPU alone.

There are **two separate software stacks** for the RK3588 NPU, each with different kernel requirements:

| Stack | Kernel | License | Capabilities |
|-------|--------|---------|-------------|
| **Rocket + Teflon** (open-source) | Mainline 6.18+ | GPL / MIT | TFLite quantized CNN inference (limited ops) |
| **RKNN-Toolkit2** (proprietary) | Vendor BSP only | Proprietary | Full inference: YOLO, LLM, speech, multimodal |
{.dense}

> BredOS ships a **mainline kernel** by default. The open-source Rocket + Teflon stack works out of the box on BredOS kernels 6.18 and later. The proprietary RKNN-Toolkit2 requires a **vendor BSP kernel** (e.g., Rockchip's `linux-rockchip-rkr3`) which is **not available on BredOS**. See section [7. Proprietary Stack (RKNN-Toolkit2)](#h-7-proprietary-stack-rknn-toolkit2) for details.
{.is-warning}

# 2. Supported Hardware

- The following table shows which Rockchip SoCs include an NPU supported by the open-source Rocket driver:

| SoC | NPU Cores | Performance | Kernel Support |
|-----|-----------|-------------|----------------|
| RK3588 / RK3588S | 3 | 6 TOPS | 6.18+ |
{.dense}

> The Rocket driver currently supports only the `RK3588` family. Support for additional Rockchip SoCs (RK3576, RK3566/RK3568) is planned upstream.
{.is-info}

All BredOS-supported boards with an `RK3588` or `RK3588S` SoC can use the NPU. This includes the Rock 5B, Rock 5B Plus, Orange Pi 5 series, and others listed on the [supported devices](/en/table-of-supported-devices) page.

# 3. Software Stack (Open-Source)

The open-source NPU stack has two components:

## 3.1 Kernel Driver (Rocket)

The `Rocket` driver is an accelerator driver (`accel` subsystem) that manages NPU hardware: powering it on/off, allocating memory buffers, and submitting jobs. It exposes the device at `/dev/accel/accel0`.

The driver was developed by [Tomeu Vizoso](https://blog.tomeuvizoso.net/) and merged into mainline Linux `6.18`. BredOS kernels `6.18` and later include it by default.

## 3.2 Userspace (Mesa Teflon)

`Teflon` is a TensorFlow Lite external delegate included in Mesa. It translates TFLite model operations into NPU jobs via the Rocket Gallium driver.

BredOS ships Mesa with Teflon support built-in. The delegate library is located at `/usr/lib/libteflon.so`.

# 4. Setup

## 4.1 Verify Kernel Support

- Check that the `Rocket` module is loaded:

```
lsmod | grep rocket
```

You should see `rocket` in the output. If the module is not loaded, load it manually:

- Load the Rocket module:

```
sudo modprobe rocket
```

- Verify the NPU device node exists:

```
ls -l /dev/accel/accel0
```

> If `/dev/accel/accel0` does not exist, your kernel may be older than `6.18` or missing the `CONFIG_DRM_ACCEL_ROCKET` option. Update to a recent BredOS kernel.
{.is-warning}

## 4.2 Install Userspace Packages

The NPU inference stack requires `Python 3.11`, `tflite-runtime`, and `numpy`. BredOS currently ships Python `3.14` as the system default, but `tflite-runtime` only provides wheels up to Python `3.11`.

- Install Python 3.11 and pip:

```
sudo pacman -S python311
```

- Install TFLite Runtime and NumPy:

```
python3.11 -m pip install --user "numpy<2" tflite-runtime
```

> The `numpy<2` constraint is required because `tflite-runtime 2.14` is not compatible with NumPy 2.x.
{.is-warning}

- Optionally, install Pillow for image preprocessing:

```
python3.11 -m pip install --user Pillow
```

## 4.3 Verify Teflon Delegate

- Confirm that `libteflon.so` is available:

```
ls /usr/lib/libteflon.so
```

If the file is missing, update Mesa:

- Update Mesa to a version with Teflon support:

```
sudo pacman -Syu mesa
```

# 5. Running Inference

This section demonstrates image classification using MobileNet V1 on the NPU.

## 5.1 Download a Model and Labels

- Download the quantized MobileNet V1 model and labels:

```
wget https://storage.googleapis.com/download.tensorflow.org/models/mobilenet_v1_2018_08_02/mobilenet_v1_1.0_224_quant.tgz
tar xzf mobilenet_v1_1.0_224_quant.tgz
wget https://storage.googleapis.com/download.tensorflow.org/models/mobilenet_v1_1.0_224_quant_and_labels.zip
unzip mobilenet_v1_1.0_224_quant_and_labels.zip
```

## 5.2 Classify an Image

- Create a script called `classify.py`:

```python
import numpy as np
from PIL import Image
import tflite_runtime.interpreter as tflite

# Load the Teflon delegate for NPU acceleration
delegate = tflite.load_delegate("/usr/lib/libteflon.so")
interpreter = tflite.Interpreter(
    model_path="mobilenet_v1_1.0_224_quant.tflite",
    experimental_delegates=[delegate]
)
interpreter.allocate_tensors()

# Preprocess image
img = Image.open("your_image.jpg").resize((224, 224))
input_data = np.expand_dims(np.array(img, dtype=np.uint8), axis=0)

# Run inference
interpreter.set_tensor(interpreter.get_input_details()[0]["index"], input_data)
interpreter.invoke()
output = interpreter.get_tensor(interpreter.get_output_details()[0]["index"])

# Show top result
with open("labels_mobilenet_quant_v1_224.txt") as f:
    labels = f.read().splitlines()
top = np.argmax(output[0])
print(f"Prediction: {labels[top]} ({output[0][top] / 255:.1%})")
```

- Run the script with Python 3.11:

```
python3.11 classify.py
```

## 5.3 CPU vs NPU Comparison

To verify that the NPU is actually being used, you can run inference with and without the delegate.

- Without the delegate (CPU only), change the interpreter initialization to:

```python
interpreter = tflite.Interpreter(model_path="mobilenet_v1_1.0_224_quant.tflite")
```

- A typical comparison on RK3588 with MobileNet V1 quantized:

| Method | Inference Time |
|--------|---------------|
| CPU (Cortex-A76) | ~48 ms |
| NPU (Teflon delegate) | ~13 ms |
{.dense}

The NPU provides roughly a 3-4x speedup for this model.

# 6. Capabilities and Limitations

## 6.1 What the Open-Source Stack Supports

The Rocket + Teflon stack supports the following TFLite operations on the NPU:

- Convolutions (most configurations)
- Tensor additions
- ReLU activation (fused with convolutions)
- Quantized (`uint8`) models

Models that have been tested successfully include `MobileNetV1`, `MobileNetV2`, and `MobileDet`.

## 6.2 Current Limitations of the Open-Source Stack

- **Quantized models only** — The NPU hardware operates on fixed-point arithmetic. Floating-point models run entirely on the CPU.
- **Limited operations** — Only convolution, addition, and fused ReLU are offloaded to the NPU. Unsupported operations fall back to CPU automatically.
- **No advanced activations** — Operations like SiLU (used in YOLOv8) are not yet implemented.
- **Single-core execution** — While the RK3588 has 3 NPU cores, the current driver uses only one core at a time.
- **CNN-focused** — The stack is optimized for convolutional neural networks. Transformer-based models are not accelerated.
- **Early-stage performance** — The open-source stack does not yet match the proprietary RKNN driver in throughput.

> The Teflon delegate automatically falls back to CPU for unsupported operations, so models with mixed operations will still run correctly, just with partial acceleration.
{.is-info}

# 7. Proprietary Stack (RKNN-Toolkit2)

## 7.1 Overview

Rockchip provides `RKNN-Toolkit2`, a proprietary SDK for NPU inference that supports significantly more operations and higher performance than the current open-source stack. It includes model conversion tools (ONNX, TensorFlow, PyTorch, TFLite to RKNN format), a C/C++ runtime library, and Python bindings.

## 7.2 Requirements

RKNN-Toolkit2 requires:

- A **vendor BSP kernel** with the proprietary `rknpu.ko` driver (e.g., Rockchip's `linux-rockchip-rkr3` or `linux-rockchip-rkr4`)
- The `rknpu2` userspace library from [rockchip-linux/rknpu2](https://github.com/rockchip-linux/rknpu2)
- Ubuntu 20.04/22.04 or Debian is the officially supported OS

> The proprietary `rknpu.ko` driver is **not included in mainline Linux** and is **not available on BredOS**. BredOS uses a mainline kernel which provides the open-source Rocket driver instead. If you need RKNN-Toolkit2, you must use a distribution that ships the vendor BSP kernel (e.g., Armbian with Rockchip BSP, Radxa's official images, or Joshua Riek's Ubuntu Rockchip).
{.is-warning}

## 7.3 Concrete Use Cases

The proprietary RKNN stack enables a much wider range of AI workloads than the open-source stack. Here are concrete use cases with software:

### Object Detection

| Project | Model | Performance | Link |
|---------|-------|-------------|------|
| rknn_model_zoo | YOLOv5, v8, v10, v11 | 50+ FPS (YOLOv8n) | [airockchip/rknn_model_zoo](https://github.com/airockchip/rknn_model_zoo) |
| rknn-cpp-yolo | YOLOv11 + RGA preproc | 25 FPS (YOLOv11s) | [yuunnn-w/rknn-cpp-yolo](https://github.com/yuunnn-w/rknn-cpp-yolo) |
| YoloV5-NPU | YOLOv5/v6/v7/v8 | 53 FPS (YOLOv8n) | [Qengineering/YoloV5-NPU](https://github.com/Qengineering/YoloV5-NPU) |
| ros_rknn_yolo | YOLO as ROS node | Real-time | [BluewhaleRobot/ros_rknn_yolo](https://github.com/BluewhaleRobot/ros_rknn_yolo) |
{.dense}

### LLM and Multimodal Inference

Rockchip provides [rknn-llm](https://github.com/airockchip/rknn-llm), a dedicated toolkit for running large language models and vision-language models on the NPU:

- **Qwen2-VL**: multimodal vision-language model running entirely on NPU
- **DeepSeek-R1-Distill-Qwen-1.5B**: distilled reasoning model
- **rkllm_server**: local API server for LLM inference
- **Multimodal dialogue**: interactive text+image conversations

### Speech and Audio

Available in [rknn_model_zoo](https://github.com/airockchip/rknn_model_zoo):

- **Whisper**: speech-to-text transcription
- **Zipformer**: streaming speech recognition
- **MMS-TTS**: text-to-speech synthesis
- **Wav2Vec**: speech representation learning
- **YamNet**: audio event classification

### Other Vision Tasks

- **RetinaFace**: face detection (243 FPS with mobile320 model)
- **MobileSAM**: segment-anything on edge devices
- **CLIP**: image-text matching and zero-shot classification
- **PPOCR**: text detection and recognition (OCR)
- **YOLOv8-Pose**: human pose estimation
- **YOLOv8-OBB**: oriented bounding box detection

## 7.4 When to Use Which Stack

| Use Case | Recommended Stack |
|----------|-------------------|
| Simple CNN classification (MobileNet) | Rocket + Teflon (open-source, works on BredOS) |
| Object detection (YOLO) | RKNN-Toolkit2 (requires vendor kernel) |
| LLM inference on NPU | RKNN-LLM (requires vendor kernel) |
| Speech recognition | RKNN-Toolkit2 (requires vendor kernel) |
| Long-term mainline support | Rocket + Teflon (improving upstream) |
{.dense}

> If you need maximum NPU performance or support for complex models (YOLO, LLMs, transformers), the RKNN-Toolkit2 with a vendor BSP kernel is currently the more capable option. The open-source Rocket + Teflon stack is actively improving and is the recommended long-term path for mainline kernel users.
{.is-info}

# 8. Troubleshooting

## 8.1 No /dev/accel/accel0

- Verify the Rocket module is available in your kernel:

```
zgrep CONFIG_DRM_ACCEL_ROCKET /proc/config.gz
```

The output should show `CONFIG_DRM_ACCEL_ROCKET=m` or `CONFIG_DRM_ACCEL_ROCKET=y`. If not, you need a kernel `6.18` or later with this option enabled.

## 8.2 Teflon Delegate Fails to Load

- Check that Mesa was built with Teflon support:

```
pacman -Ql mesa | grep teflon
```

If `libteflon.so` is not listed, the installed Mesa version may not include Teflon. Update Mesa or check the BredOS repositories for an updated package.

## 8.3 tflite-runtime Installation Fails

If `pip install tflite-runtime` fails with a "no matching distribution" error, verify you are using `Python 3.11`:

- Check your Python version:

```
python3.11 --version
```

The `tflite-runtime` package does not provide wheels for all Python versions. Python `3.11` is the latest version with confirmed support.

## 8.4 RKNN-Toolkit2 Does Not Work on BredOS

This is expected. RKNN-Toolkit2 requires the proprietary `rknpu.ko` driver, which is only available in vendor BSP kernels. BredOS uses a mainline kernel with the open-source Rocket driver. To use RKNN-Toolkit2, you need a distribution with a vendor kernel (see [section 7.2](#h-72-requirements)).

# 9. References

- [Rockchip NPU update 6: We are in mainline!](https://blog.tomeuvizoso.net/2025/07/rockchip-npu-update-6-we-are-in-mainline.html) - Tomeu Vizoso
- [accel/rocket kernel documentation](https://docs.kernel.org/accel/rocket/index.html) - kernel.org
- [RKNN-Toolkit2](https://github.com/airockchip/rknn-toolkit2) - Rockchip/Airockchip
- [RKNN-LLM](https://github.com/airockchip/rknn-llm) - Rockchip/Airockchip (LLM inference on NPU)
- [RKNN Model Zoo](https://github.com/airockchip/rknn_model_zoo) - Rockchip/Airockchip (official demos and benchmarks)
- [rknpu2 userspace library](https://github.com/rockchip-linux/rknpu2) - Rockchip
- [Collabora RK3588 mainline status](https://gitlab.collabora.com/hardware-enablement/rockchip-3588/notes-for-rockchip-3588/-/blob/main/mainline-status.md) - Collabora
- [Running mainline Linux on Rockchip: a year in review](https://www.collabora.com/news-and-blog/blog/2026/03/02/running-mainline-linux-u-boot-and-mesa-on-rockchip-a-year-in-review/) - Collabora (FOSDEM 2026)
