/*###############################################################################
#
# Copyright 2026 Burak Aydin
#
# Permission is hereby granted, free of charge, to any person obtaining a copy of
# this software and associated documentation files (the "Software"), to deal in
# the Software without restriction, including without limitation the rights to
# use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
# the Software, and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
# FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
# COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
# IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
###############################################################################*/

/*
 * nvar_bridge — Flat C API wrapping the NVIDIA Maxine AR SDK (nvAR).
 *
 * Hides CUDA memory management (NvCVImage), the string-based parameter system,
 * and SDK-specific types behind opaque handles and flat arrays.
 *
 * Callable from C, C#/P/Invoke, Python/ctypes, or any FFI-capable language.
 */

#ifndef NVAR_BRIDGE_H
#define NVAR_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*  Export / visibility                                                        */
/* -------------------------------------------------------------------------- */

#ifdef _WIN32
    #ifdef NVAR_BRIDGE_EXPORT
        #define NVAR_API __declspec(dllexport) __cdecl
    #else
        #define NVAR_API __declspec(dllimport) __cdecl
    #endif
#else
    #ifdef NVAR_BRIDGE_EXPORT
        #define NVAR_API __attribute__((visibility("default")))
    #else
        #define NVAR_API
    #endif
#endif

/* -------------------------------------------------------------------------- */
/*  Opaque handles                                                             */
/* -------------------------------------------------------------------------- */

typedef struct NvarSession NvarSession;
typedef struct NvarFeature NvarFeature;

/* -------------------------------------------------------------------------- */
/*  Enumerations                                                               */
/* -------------------------------------------------------------------------- */

/* Pixel formats accepted by nvar_push_frame(). */
enum NvarPixelFormat {
    NVAR_FORMAT_RGB  = 0,   /* 3 bytes/pixel, RGB order */
    NVAR_FORMAT_BGR  = 1,   /* 3 bytes/pixel, BGR order */
    NVAR_FORMAT_RGBA = 2    /* 4 bytes/pixel, RGBA order */
};

/* Error codes returned by all bridge functions. */
enum NvarResult {
    NVAR_OK                   =  0,
    NVAR_ERR_INVALID_ARG      = -1,
    NVAR_ERR_NULL_HANDLE      = -2,
    NVAR_ERR_SDK              = -3,  /* nvAR SDK error (detail via nvar_get_last_error) */
    NVAR_ERR_CUDA             = -4,
    NVAR_ERR_NOT_LOADED       = -5,  /* feature not yet loaded */
    NVAR_ERR_NO_FRAME         = -6,  /* no frame pushed yet */
    NVAR_ERR_BUFFER_TOO_SMALL = -7,  /* caller's output buffer too small */
    NVAR_ERR_NOT_AVAILABLE    = -8,  /* feature doesn't produce this output */
    NVAR_ERR_LIBRARY          = -9   /* SDK DLL/SO not found */
};

/* Feature IDs — pass to nvar_create_feature().
 * Use the string constants below; they match the SDK's NvAR_Feature_* defines:
 *   "FaceBoxDetection", "LandmarkDetection", "Face3DReconstruction",
 *   "FaceExpressions", "BodyDetection", "BodyPoseEstimation",
 *   "GazeRedirection"
 */

/* -------------------------------------------------------------------------- */
/*  Session management                                                         */
/* -------------------------------------------------------------------------- */

/*
 * Create a session: allocates CUDA stream and sets the model directory.
 *
 * out_session  receives the new session handle
 * model_dir    path to the directory containing TRT model files
 * cuda_device  GPU index (0 for default)
 *
 * Returns NVAR_OK on success, NVAR_ERR_LIBRARY if SDK DLLs cannot be loaded.
 */
int NVAR_API nvar_create_session(NvarSession** out_session,
                                 const char* model_dir,
                                 int cuda_device);

/*
 * Destroy a session and all features created within it.
 * Passing NULL is a safe no-op.
 */
int NVAR_API nvar_destroy_session(NvarSession* session);

/*
 * Push a video frame from CPU memory into the session's GPU buffer.
 *
 * The bridge handles NvCVImage allocation, resizing, format conversion,
 * and CPU-to-GPU transfer. If dimensions change, buffers are reallocated.
 *
 * pixels   raw pixel bytes in CPU memory
 * width    image width in pixels
 * height   image height in pixels
 * format   one of NvarPixelFormat
 */
int NVAR_API nvar_push_frame(NvarSession* session,
                              const void* pixels,
                              unsigned int width,
                              unsigned int height,
                              int format);

/* -------------------------------------------------------------------------- */
/*  Feature management                                                         */
/* -------------------------------------------------------------------------- */

/*
 * Create a feature handle within a session.
 *
 * feature_id   SDK feature string, e.g. "FaceExpressions"
 * out_feature  receives the new feature handle
 */
int NVAR_API nvar_create_feature(NvarSession* session,
                                  const char* feature_id,
                                  NvarFeature** out_feature);

/*
 * Destroy a feature handle. Passing NULL is a safe no-op.
 */
int NVAR_API nvar_destroy_feature(NvarFeature* feature);

/*
 * Configure a feature parameter BEFORE calling nvar_load().
 * These are pass-through to NvAR_SetU32/SetString/SetF32.
 */
int NVAR_API nvar_set_u32(NvarFeature* feature, const char* param_name, unsigned int value);
int NVAR_API nvar_set_string(NvarFeature* feature, const char* param_name, const char* value);
int NVAR_API nvar_set_f32(NvarFeature* feature, const char* param_name, float value);

/*
 * Load the feature model.
 * Sets model directory, CUDA stream, binds input image, queries output sizes,
 * allocates output buffers, and binds them to the SDK handle.
 *
 * Must be called after all nvar_set_* configuration and before nvar_run().
 */
int NVAR_API nvar_load(NvarFeature* feature);

/*
 * Run the feature on the current frame.
 * nvar_push_frame() must have been called on the session beforehand.
 */
int NVAR_API nvar_run(NvarFeature* feature);

/*
 * Query a feature property (available after nvar_load).
 * Pass-through to NvAR_GetU32.
 */
int NVAR_API nvar_get_u32(NvarFeature* feature, const char* param_name, unsigned int* out_value);

/* -------------------------------------------------------------------------- */
/*  Output getters (call after nvar_run)                                       */
/*                                                                             */
/*  Each getter copies results from internal buffers to caller-provided arrays.*/
/*  The 'count' parameter is in/out: caller sets max capacity, bridge sets     */
/*  actual count. Returns NVAR_ERR_BUFFER_TOO_SMALL if capacity is too small.  */
/*  Returns NVAR_ERR_NOT_AVAILABLE if the feature doesn't produce this output. */
/* -------------------------------------------------------------------------- */

/* Face: expression coefficients (52-53 ARKit blendshapes depending on model). */
int NVAR_API nvar_get_expressions(NvarFeature* feature, float* out, unsigned int* count);

/* Face: shape identity eigenvalues (Face3DReconstruction only). */
int NVAR_API nvar_get_eigenvalues(NvarFeature* feature, float* out, unsigned int* count);

/* Face: 3D mesh vertices as flat [x,y,z, x,y,z, ...] (Face3DReconstruction). */
int NVAR_API nvar_get_mesh_vertices(NvarFeature* feature, float* out, unsigned int* vertex_count);

/* Face: mesh triangle indices as flat [v0,v1,v2, ...] (Face3DReconstruction). */
int NVAR_API nvar_get_mesh_triangles(NvarFeature* feature, unsigned short* out, unsigned int* triangle_count);

/* Face/Body: 2D landmarks as flat [x,y, x,y, ...]. */
int NVAR_API nvar_get_landmarks_2d(NvarFeature* feature, float* out, unsigned int* count);

/* Face: 3D landmarks as flat [x,y,z, x,y,z, ...]. */
int NVAR_API nvar_get_landmarks_3d(NvarFeature* feature, float* out, unsigned int* count);

/* Face/Body: landmark confidence scores. */
int NVAR_API nvar_get_landmarks_confidence(NvarFeature* feature, float* out, unsigned int* count);

/* Face: head pose (rotation as quaternion xyzw + translation xyz = 7 floats). */
int NVAR_API nvar_get_pose(NvarFeature* feature, float* out_rotation_xyzw, float* out_translation_xyz);

/* Face/Body: bounding boxes as flat [x,y,w,h, x,y,w,h, ...]. */
int NVAR_API nvar_get_bounding_boxes(NvarFeature* feature, float* out, unsigned int* count);

/* Body: 2D keypoints as flat [x,y, x,y, ...]. */
int NVAR_API nvar_get_body_keypoints_2d(NvarFeature* feature, float* out, unsigned int* count);

/* Body: 3D keypoints as flat [x,y,z, x,y,z, ...]. */
int NVAR_API nvar_get_body_keypoints_3d(NvarFeature* feature, float* out, unsigned int* count);

/* Body: joint angles as flat array. */
int NVAR_API nvar_get_body_joint_angles(NvarFeature* feature, float* out, unsigned int* count);

/* Body: tracking bounding boxes as flat [x,y,w,h,tracking_id, ...]. */
int NVAR_API nvar_get_tracking_bboxes(NvarFeature* feature, float* out, unsigned int* count);

/* Gaze: angles (2 floats: pitch, yaw). */
int NVAR_API nvar_get_gaze_angles(NvarFeature* feature, float* out);

/* Gaze: head pose as quaternion xyzw (4 floats). */
int NVAR_API nvar_get_gaze_head_pose(NvarFeature* feature, float* out_rotation_xyzw);

/* Gaze: head translation (3 floats: x, y, z). */
int NVAR_API nvar_get_gaze_head_translation(NvarFeature* feature, float* out_xyz);

/* -------------------------------------------------------------------------- */
/*  Error handling                                                             */
/* -------------------------------------------------------------------------- */

/* Return a static error string for a NvarResult code. */
const char* NVAR_API nvar_get_error_string(int result);

/* Return the thread-local detail string (set by the most recent failed call). */
const char* NVAR_API nvar_get_last_error(void);

/* -------------------------------------------------------------------------- */
/*  SDK version                                                                */
/* -------------------------------------------------------------------------- */

/*
 * Get the SDK version number.
 * Encoded as (major << 24) | (minor << 16) | (build << 8) | 0.
 */
int NVAR_API nvar_get_sdk_version(unsigned int* out_version);

#ifdef __cplusplus
}
#endif

#endif /* NVAR_BRIDGE_H */
