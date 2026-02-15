# NPU (Neural Processing Unit) - Rock 5B+ Mainline Kernel

## Overview

The RK3588 includes a 6 TOPS NPU (Neural Processing Unit) with 3 cores, marketed by Rockchip as RKNN/RKNPU. Upstream kernel support is provided by the **Rocket** driver (`drivers/accel/rocket/`), authored by Tomeu Vizoso. The userspace interface is implemented via Mesa's **Teflon** TFLite delegate.

- **Kernel driver**: `CONFIG_DRM_ACCEL_ROCKET=m` (available since kernel 6.18)
- **Userspace**: Mesa's `libteflon.so` (TFLite delegate for NPU inference)
- **Device**: `/dev/accel/accel0`

## Kernel Configuration

Two config options are required:

```
CONFIG_DRM_ACCEL=y          # DRM accelerator subsystem (built into drm.ko)
CONFIG_DRM_ACCEL_ROCKET=m   # Rocket NPU driver (rocket.ko module)
```

### Dependencies

The Rocket driver requires (automatically selected):
- `DRM_SCHED` - DRM GPU scheduler
- `DRM_GEM_SHMEM_HELPER` - GEM shared memory helpers
- `ROCKCHIP_IOMMU` - Rockchip IOMMU

### Important: Full Kernel Rebuild Required

Enabling `CONFIG_DRM_ACCEL=y` changes `drm.ko` (the DRM subsystem module). You **cannot** just build `rocket.ko` separately with `make M=drivers/accel/rocket` — it will fail with undefined symbol `accel_open`. A full kernel rebuild (Image + all modules) is required.

```bash
# Full rebuild
make KBUILD_MODPOST_WARN=1 -j$(nproc) Image modules
```

## Verification

After booting the new kernel:

```bash
# Check driver loaded
lsmod | grep rocket

# Check device exists
ls -la /dev/accel/accel0

# Check kernel log (all 3 cores should be detected)
journalctl -k | grep rocket
# Expected output:
# [drm] Initialized rocket 0.0.0 for rknn on minor 0
# rocket fdab0000.npu: Rockchip NPU core 0 version: 1179210309
# rocket fdac0000.npu: Rockchip NPU core 1 version: 1179210309
# rocket fdad0000.npu: Rockchip NPU core 2 version: 1179210309
```

## Userspace Setup (BredOS)

### Prerequisites

BredOS with Mesa >= 25.3 already includes `libteflon.so`:

```bash
ls -la /usr/lib/libteflon.so
# Should exist if Mesa is built with Teflon support
```

### Install TFLite Runtime

As of February 2026, `tflite-runtime` has no wheel for Python 3.14. Use Python 3.11:

```bash
# Install Python 3.11
sudo pacman -S python311

# Bootstrap pip
python3.11 -m ensurepip --user

# Install tflite-runtime and dependencies
python3.11 -m pip install --user tflite-runtime==2.14.0 numpy==1.26.4 Pillow
```

**Note**: Use `numpy<2` — tflite-runtime 2.14.0 was compiled against NumPy 1.x and crashes with NumPy 2.x.

### Test Inference

Download a quantized MobileNet V1 model for testing:

```bash
mkdir -p ~/npu-test && cd ~/npu-test

curl -sLO https://storage.googleapis.com/download.tensorflow.org/models/mobilenet_v1_2018_08_02/mobilenet_v1_1.0_224_quant.tgz
tar xzf mobilenet_v1_1.0_224_quant.tgz mobilenet_v1_1.0_224_quant.tflite

curl -sLO https://storage.googleapis.com/download.tensorflow.org/models/mobilenet_v1_1.0_224_quant_and_labels.zip
python3.11 -c "import zipfile; zipfile.ZipFile('mobilenet_v1_1.0_224_quant_and_labels.zip').extract('labels_mobilenet_quant_v1_224.txt')"

curl -sLO https://raw.githubusercontent.com/tensorflow/tensorflow/master/tensorflow/lite/examples/label_image/testdata/grace_hopper.bmp
```

Run classification with NPU:

```python
#!/usr/bin/env python3.11
"""NPU inference test using Mesa Teflon delegate."""
import time
import numpy as np
from PIL import Image
import tflite_runtime.interpreter as tflite

# Load model with Teflon NPU delegate
delegate = [tflite.load_delegate("/usr/lib/libteflon.so")]
interpreter = tflite.Interpreter(
    model_path="mobilenet_v1_1.0_224_quant.tflite",
    experimental_delegates=delegate
)
interpreter.allocate_tensors()

# Preprocess image
img = Image.open("grace_hopper.bmp").resize((224, 224))
input_data = np.expand_dims(np.array(img, dtype=np.uint8), axis=0)

# Run inference
input_details = interpreter.get_input_details()
output_details = interpreter.get_output_details()
interpreter.set_tensor(input_details[0]['index'], input_data)

t0 = time.perf_counter()
interpreter.invoke()
elapsed = time.perf_counter() - t0

# Results
output = interpreter.get_tensor(output_details[0]['index'])[0]
with open("labels_mobilenet_quant_v1_224.txt") as f:
    labels = [l.strip() for l in f]
top = np.argsort(output)[-5:][::-1]
print(f"Inference time: {elapsed*1000:.1f} ms")
for i, idx in enumerate(top):
    print(f"  {i+1}. {labels[idx]} (score: {output[idx]})")
```

## Performance Results

Tested on Rock 5B+ with MobileNet V1 quantized (224x224), averaged over 10 runs:

| Backend | Avg Inference | Speedup |
|---------|--------------|---------|
| CPU (TFLite) | 47.7 ms | 1.0x |
| NPU (Teflon) | 12.6 ms | 3.8x |

Both backends correctly classify the test image as "military uniform" (Admiral Grace Hopper).

## References

- Rocket driver upstream commit: kernel 6.18 (Tomeu Vizoso)
- Mesa Teflon delegate: `src/gallium/frontends/teflon/` in Mesa source
- UAPI header: `include/uapi/drm/rocket_accel.h`
