# nvar_bridge

Thin C bridge library wrapping the [NVIDIA Maxine AR SDK](https://developer.nvidia.com/maxine) (`nvAR`).

Hides CUDA memory management (`NvCVImage`), the string-based parameter system, and SDK-specific types behind a flat `extern "C"` API with opaque handles. Callable from C, C#/P/Invoke, Python/ctypes, Unity, or any FFI-capable language.

## Design

- All functions return `int` (0 = success, negative = error)
- Opaque handles: `NvarSession*`, `NvarFeature*`
- `extern "C"` with `__cdecl` calling convention on Windows
- No C++ types in the public API
- Thread-local error string via `nvar_get_last_error()`
- Bridge owns all GPU memory; callers only see flat arrays
- No link-time dependency on SDK — libraries loaded lazily at runtime

## Supported Features

All 7 AR SDK features are supported:

| Feature ID | Description |
|---|---|
| `FaceBoxDetection` | Face bounding box detection |
| `LandmarkDetection` | 2D facial landmark detection |
| `Face3DReconstruction` | Full 3D face mesh, expressions, eigenvalues |
| `FaceExpressions` | ARKit blendshape coefficients (52-53) |
| `BodyDetection` | Body bounding box detection |
| `BodyPoseEstimation` | 2D/3D body keypoints, joint angles, tracking |
| `GazeRedirection` | Gaze direction, head pose |

## Build

```bash
cd /path/to/Maxine-AR-SDK
cmake -B build -DBUILD_BRIDGE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target nvar_bridge --config Release
```

Output: `libnvar_bridge.so` (Linux) or `nvar_bridge.dll` (Windows).

The build does **not** require the NVIDIA AR SDK to be installed — the bridge loads SDK libraries (`nvARPose`, `NVCVImage`) lazily at runtime.

## API Reference

### Session Management

```c
// Create a session (allocates CUDA stream, sets model directory)
int nvar_create_session(NvarSession** out, const char* model_dir, int cuda_device);

// Destroy session and all associated resources
int nvar_destroy_session(NvarSession* session);

// Push a video frame (CPU pixels -> GPU, handles format conversion)
int nvar_push_frame(NvarSession* session, const void* pixels,
                    unsigned int width, unsigned int height, int format);
```

### Feature Lifecycle

```c
// Create a feature (e.g. "FaceExpressions")
int nvar_create_feature(NvarSession* session, const char* feature_id, NvarFeature** out);

// Configure before loading
int nvar_set_u32(NvarFeature* feature, const char* param, unsigned int value);
int nvar_set_string(NvarFeature* feature, const char* param, const char* value);
int nvar_set_f32(NvarFeature* feature, const char* param, float value);

// Load model (allocates output buffers)
int nvar_load(NvarFeature* feature);

// Run inference on current frame
int nvar_run(NvarFeature* feature);

// Clean up
int nvar_destroy_feature(NvarFeature* feature);
```

### Output Getters

All getters copy from internal buffers to caller-provided arrays. The `count` parameter is in/out: set max capacity, receives actual count.

```c
// Face expressions (52-53 ARKit blendshapes)
int nvar_get_expressions(NvarFeature* f, float* out, unsigned int* count);

// Face mesh (Face3DReconstruction)
int nvar_get_mesh_vertices(NvarFeature* f, float* out, unsigned int* vertex_count);
int nvar_get_mesh_triangles(NvarFeature* f, unsigned short* out, unsigned int* triangle_count);
int nvar_get_eigenvalues(NvarFeature* f, float* out, unsigned int* count);

// Landmarks
int nvar_get_landmarks_2d(NvarFeature* f, float* out, unsigned int* count);
int nvar_get_landmarks_3d(NvarFeature* f, float* out, unsigned int* count);
int nvar_get_landmarks_confidence(NvarFeature* f, float* out, unsigned int* count);

// Pose
int nvar_get_pose(NvarFeature* f, float* rotation_xyzw, float* translation_xyz);

// Bounding boxes
int nvar_get_bounding_boxes(NvarFeature* f, float* out, unsigned int* count);

// Body (BodyPoseEstimation)
int nvar_get_body_keypoints_2d(NvarFeature* f, float* out, unsigned int* count);
int nvar_get_body_keypoints_3d(NvarFeature* f, float* out, unsigned int* count);
int nvar_get_body_joint_angles(NvarFeature* f, float* out, unsigned int* count);
int nvar_get_tracking_bboxes(NvarFeature* f, float* out, unsigned int* count);

// Gaze (GazeRedirection)
int nvar_get_gaze_angles(NvarFeature* f, float* out);          // 2 floats: pitch, yaw
int nvar_get_gaze_head_pose(NvarFeature* f, float* out_xyzw);  // quaternion
int nvar_get_gaze_head_translation(NvarFeature* f, float* out); // 3 floats
```

### Error Handling

```c
const char* nvar_get_error_string(int result);  // Static string for error code
const char* nvar_get_last_error(void);           // Thread-local detail string
int nvar_get_sdk_version(unsigned int* version); // SDK version
```

## Usage Examples

### C

```c
#include "nvar_bridge.h"
#include <stdio.h>

int main() {
    NvarSession* session = NULL;
    NvarFeature* expr = NULL;

    if (nvar_create_session(&session, "/path/to/models", 0) != NVAR_OK) {
        printf("Error: %s\n", nvar_get_last_error());
        return 1;
    }

    nvar_create_feature(session, "FaceExpressions", &expr);
    nvar_set_u32(expr, "NvAR_Parameter_Config_Temporal", 1);
    nvar_load(expr);

    // For each frame:
    unsigned char* pixels = /* your frame data */;
    nvar_push_frame(session, pixels, 640, 480, NVAR_FORMAT_BGR);
    nvar_run(expr);

    float coeffs[53];
    unsigned int count = 53;
    nvar_get_expressions(expr, coeffs, &count);
    printf("Got %u expression coefficients\n", count);

    nvar_destroy_feature(expr);
    nvar_destroy_session(session);
    return 0;
}
```

### C# / P/Invoke

```csharp
using System.Runtime.InteropServices;

static class NvarBridge
{
    [DllImport("nvar_bridge")] static extern int nvar_create_session(out IntPtr session, string modelDir, int device);
    [DllImport("nvar_bridge")] static extern int nvar_destroy_session(IntPtr session);
    [DllImport("nvar_bridge")] static extern int nvar_push_frame(IntPtr session, byte[] pixels, uint w, uint h, int fmt);
    [DllImport("nvar_bridge")] static extern int nvar_create_feature(IntPtr session, string id, out IntPtr feature);
    [DllImport("nvar_bridge")] static extern int nvar_destroy_feature(IntPtr feature);
    [DllImport("nvar_bridge")] static extern int nvar_load(IntPtr feature);
    [DllImport("nvar_bridge")] static extern int nvar_run(IntPtr feature);
    [DllImport("nvar_bridge")] static extern int nvar_get_expressions(IntPtr feature, float[] buf, ref uint count);
    [DllImport("nvar_bridge")] static extern IntPtr nvar_get_last_error();
}
```

### Python / ctypes

```python
import ctypes

lib = ctypes.CDLL("./libnvar_bridge.so")
session = ctypes.c_void_p()
lib.nvar_create_session(ctypes.byref(session), b"/path/to/models", 0)

feature = ctypes.c_void_p()
lib.nvar_create_feature(session, b"FaceExpressions", ctypes.byref(feature))
lib.nvar_load(feature)

# Push frame (BGR u8 pixels)
lib.nvar_push_frame(session, pixels_ptr, 640, 480, 1)  # NVAR_FORMAT_BGR
lib.nvar_run(feature)

coeffs = (ctypes.c_float * 53)()
count = ctypes.c_uint(53)
lib.nvar_get_expressions(feature, coeffs, ctypes.byref(count))

lib.nvar_destroy_feature(feature)
lib.nvar_destroy_session(session)
```

## Pixel Formats

| Enum | Value | Description |
|---|---|---|
| `NVAR_FORMAT_RGB` | 0 | 3 bytes/pixel, RGB order |
| `NVAR_FORMAT_BGR` | 1 | 3 bytes/pixel, BGR order (SDK native) |
| `NVAR_FORMAT_RGBA` | 2 | 4 bytes/pixel, RGBA order |

The bridge converts any input format to BGR on the GPU via `NvCVImage_Transfer`.

## Thread Safety

- Sessions are **not** thread-safe. Use one session per thread, or synchronize access externally.
- Features within a session share the GPU image buffer and CUDA stream.
- `nvar_get_last_error()` is thread-local and safe to call from any thread.
- The SDK library loader is **not** thread-safe on first call. Create the first session from a single thread.

## Deployment

At runtime, the bridge requires:
- `nvARPose.dll` / `libnvARPose.so` (NVIDIA AR SDK)
- `NVCVImage.dll` / `libNVCVImage.so` (NVIDIA CV Image library)
- NVIDIA GPU driver with CUDA support

On Windows, the bridge searches:
1. Standard DLL search path
2. `NV_AR_SDK_PATH` environment variable

On Linux, ensure the SDK libraries are on `LD_LIBRARY_PATH`.

If the SDK is not installed, all functions return `NVAR_ERR_LIBRARY` (-9) with a descriptive error message.

## License

MIT License — see the header in source files.
