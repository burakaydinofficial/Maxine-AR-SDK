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

#include "nvar_bridge.h"
#include "nvAR.h"
#include "nvAR_defs.h"
#include "nvCVImage.h"
#include "nvCVStatus.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#ifdef _WIN32
    #define _WINSOCKAPI_
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

/* ========================================================================== */
/*  Platform abstraction for dynamic library loading                           */
/* ========================================================================== */

#ifdef _WIN32
    typedef HMODULE LibHandle;
    static LibHandle lib_open(const char* name)   { return LoadLibraryA(name); }
    static void*     lib_sym(LibHandle h, const char* s) { return (void*)GetProcAddress(h, s); }
    static void      lib_close(LibHandle h)       { if (h) FreeLibrary(h); }
#else
    typedef void* LibHandle;
    static LibHandle lib_open(const char* name)   { return dlopen(name, RTLD_LAZY); }
    static void*     lib_sym(LibHandle h, const char* s) { return h ? dlsym(h, s) : nullptr; }
    static void      lib_close(LibHandle h)       { if (h) dlclose(h); }
#endif

/* ========================================================================== */
/*  Thread-local error string                                                  */
/* ========================================================================== */

static thread_local char g_last_error[1024] = "";

static void set_error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_last_error, sizeof(g_last_error), fmt, args);
    va_end(args);
}

static void clear_error(void) {
    g_last_error[0] = '\0';
}

/* ========================================================================== */
/*  SDK lazy loader — loads nvARPose and NVCVImage at runtime                  */
/* ========================================================================== */

/*
 * We load the SDK shared libraries lazily so the bridge has no link-time
 * dependency on nvARPose.dll/.so or NVCVImage.dll/.so.
 * The proxy files in nvar/src/ are Windows-only, so we implement our own
 * cross-platform loader here.
 */

/* Function pointer types matching the SDK API */
typedef NvCV_Status(NvAR_API *PFN_NvAR_GetVersion)(unsigned int*);
typedef NvCV_Status(NvAR_API *PFN_NvAR_Create)(NvAR_FeatureID, NvAR_FeatureHandle*);
typedef NvCV_Status(NvAR_API *PFN_NvAR_Destroy)(NvAR_FeatureHandle);
typedef NvCV_Status(NvAR_API *PFN_NvAR_Load)(NvAR_FeatureHandle);
typedef NvCV_Status(NvAR_API *PFN_NvAR_Run)(NvAR_FeatureHandle);
typedef NvCV_Status(NvAR_API *PFN_NvAR_SetU32)(NvAR_FeatureHandle, const char*, unsigned int);
typedef NvCV_Status(NvAR_API *PFN_NvAR_SetS32)(NvAR_FeatureHandle, const char*, int);
typedef NvCV_Status(NvAR_API *PFN_NvAR_SetF32)(NvAR_FeatureHandle, const char*, float);
typedef NvCV_Status(NvAR_API *PFN_NvAR_SetF64)(NvAR_FeatureHandle, const char*, double);
typedef NvCV_Status(NvAR_API *PFN_NvAR_SetU64)(NvAR_FeatureHandle, const char*, unsigned long long);
typedef NvCV_Status(NvAR_API *PFN_NvAR_SetObject)(NvAR_FeatureHandle, const char*, void*, unsigned long);
typedef NvCV_Status(NvAR_API *PFN_NvAR_SetString)(NvAR_FeatureHandle, const char*, const char*);
typedef NvCV_Status(NvAR_API *PFN_NvAR_SetCudaStream)(NvAR_FeatureHandle, const char*, CUstream);
typedef NvCV_Status(NvAR_API *PFN_NvAR_SetF32Array)(NvAR_FeatureHandle, const char*, float*, int);
typedef NvCV_Status(NvAR_API *PFN_NvAR_GetU32)(NvAR_FeatureHandle, const char*, unsigned int*);
typedef NvCV_Status(NvAR_API *PFN_NvAR_GetS32)(NvAR_FeatureHandle, const char*, int*);
typedef NvCV_Status(NvAR_API *PFN_NvAR_GetF32)(NvAR_FeatureHandle, const char*, float*);
typedef NvCV_Status(NvAR_API *PFN_NvAR_GetObject)(NvAR_FeatureHandle, const char*, const void**, unsigned long);
typedef NvCV_Status(NvAR_API *PFN_NvAR_GetString)(NvAR_FeatureHandle, const char*, const char**);
typedef NvCV_Status(NvAR_API *PFN_NvAR_CudaStreamCreate)(CUstream*);
typedef NvCV_Status(NvAR_API *PFN_NvAR_CudaStreamDestroy)(CUstream);

typedef NvCV_Status(NvCV_API *PFN_NvCVImage_Init)(NvCVImage*, unsigned, unsigned, int, void*,
    NvCVImage_PixelFormat, NvCVImage_ComponentType, unsigned, unsigned);
typedef NvCV_Status(NvCV_API *PFN_NvCVImage_Alloc)(NvCVImage*, unsigned, unsigned,
    NvCVImage_PixelFormat, NvCVImage_ComponentType, unsigned, unsigned, unsigned);
typedef NvCV_Status(NvCV_API *PFN_NvCVImage_Realloc)(NvCVImage*, unsigned, unsigned,
    NvCVImage_PixelFormat, NvCVImage_ComponentType, unsigned, unsigned, unsigned);
typedef void(NvCV_API *PFN_NvCVImage_Dealloc)(NvCVImage*);
typedef NvCV_Status(NvCV_API *PFN_NvCVImage_Transfer)(const NvCVImage*, NvCVImage*, float,
    struct CUstream_st*, NvCVImage*);
typedef const char*(
#ifdef _WIN32
    __cdecl
#endif
    *PFN_NvCV_GetErrorStringFromCode)(NvCV_Status);

struct SdkApi {
    LibHandle ar_lib;
    LibHandle cv_lib;

    /* nvAR functions */
    PFN_NvAR_GetVersion         GetVersion;
    PFN_NvAR_Create             Create;
    PFN_NvAR_Destroy            Destroy;
    PFN_NvAR_Load               Load;
    PFN_NvAR_Run                Run;
    PFN_NvAR_SetU32             SetU32;
    PFN_NvAR_SetS32             SetS32;
    PFN_NvAR_SetF32             SetF32;
    PFN_NvAR_SetF64             SetF64;
    PFN_NvAR_SetU64             SetU64;
    PFN_NvAR_SetObject          SetObject;
    PFN_NvAR_SetString          SetString;
    PFN_NvAR_SetCudaStream      SetCudaStream;
    PFN_NvAR_SetF32Array        SetF32Array;
    PFN_NvAR_GetU32             GetU32;
    PFN_NvAR_GetS32             GetS32;
    PFN_NvAR_GetF32             GetF32;
    PFN_NvAR_GetObject          GetObject;
    PFN_NvAR_GetString          GetString;
    PFN_NvAR_CudaStreamCreate   CudaStreamCreate;
    PFN_NvAR_CudaStreamDestroy  CudaStreamDestroy;

    /* NvCVImage functions */
    PFN_NvCVImage_Init          ImageInit;
    PFN_NvCVImage_Alloc         ImageAlloc;
    PFN_NvCVImage_Realloc       ImageRealloc;
    PFN_NvCVImage_Dealloc       ImageDealloc;
    PFN_NvCVImage_Transfer      ImageTransfer;
    PFN_NvCV_GetErrorStringFromCode GetErrorString;

    bool loaded;
};

static SdkApi g_sdk = {};

#define LOAD_AR_SYM(field, name) do {                                          \
    g_sdk.field = reinterpret_cast<decltype(g_sdk.field)>(                      \
        lib_sym(g_sdk.ar_lib, #name));                                         \
    if (!g_sdk.field) {                                                        \
        set_error("Failed to load symbol " #name " from nvARPose library");    \
        return NVAR_ERR_LIBRARY;                                               \
    }                                                                          \
} while (0)

#define LOAD_CV_SYM(field, name) do {                                          \
    g_sdk.field = reinterpret_cast<decltype(g_sdk.field)>(                      \
        lib_sym(g_sdk.cv_lib, #name));                                         \
    if (!g_sdk.field) {                                                        \
        set_error("Failed to load symbol " #name " from NVCVImage library");   \
        return NVAR_ERR_LIBRARY;                                               \
    }                                                                          \
} while (0)

static int ensure_sdk_loaded() {
    if (g_sdk.loaded) return NVAR_OK;

#ifdef _WIN32
    g_sdk.ar_lib = lib_open("nvARPose.dll");
    if (!g_sdk.ar_lib) {
        /* Try the full path from environment */
        char path[1024] = {};
        DWORD len = GetEnvironmentVariableA("NV_AR_SDK_PATH", path, sizeof(path));
        if (len > 0 && len < sizeof(path) - 20) {
            strcat(path, "\\nvARPose.dll");
            g_sdk.ar_lib = lib_open(path);
        }
    }
    g_sdk.cv_lib = lib_open("NVCVImage.dll");
    if (!g_sdk.cv_lib) {
        char path[1024] = {};
        DWORD len = GetEnvironmentVariableA("NV_AR_SDK_PATH", path, sizeof(path));
        if (len > 0 && len < sizeof(path) - 20) {
            strcat(path, "\\NVCVImage.dll");
            g_sdk.cv_lib = lib_open(path);
        }
    }
#else
    g_sdk.ar_lib = lib_open("libnvARPose.so");
    g_sdk.cv_lib = lib_open("libNVCVImage.so");
#endif

    if (!g_sdk.ar_lib) {
        set_error("Failed to load nvARPose library. Ensure the NVIDIA AR SDK is installed.");
        return NVAR_ERR_LIBRARY;
    }
    if (!g_sdk.cv_lib) {
        set_error("Failed to load NVCVImage library. Ensure the NVIDIA AR SDK is installed.");
        return NVAR_ERR_LIBRARY;
    }

    /* Load nvAR function pointers */
    LOAD_AR_SYM(GetVersion,        NvAR_GetVersion);
    LOAD_AR_SYM(Create,            NvAR_Create);
    LOAD_AR_SYM(Destroy,           NvAR_Destroy);
    LOAD_AR_SYM(Load,              NvAR_Load);
    LOAD_AR_SYM(Run,               NvAR_Run);
    LOAD_AR_SYM(SetU32,            NvAR_SetU32);
    LOAD_AR_SYM(SetS32,            NvAR_SetS32);
    LOAD_AR_SYM(SetF32,            NvAR_SetF32);
    LOAD_AR_SYM(SetF64,            NvAR_SetF64);
    LOAD_AR_SYM(SetU64,            NvAR_SetU64);
    LOAD_AR_SYM(SetObject,         NvAR_SetObject);
    LOAD_AR_SYM(SetString,         NvAR_SetString);
    LOAD_AR_SYM(SetCudaStream,     NvAR_SetCudaStream);
    LOAD_AR_SYM(SetF32Array,       NvAR_SetF32Array);
    LOAD_AR_SYM(GetU32,            NvAR_GetU32);
    LOAD_AR_SYM(GetS32,            NvAR_GetS32);
    LOAD_AR_SYM(GetF32,            NvAR_GetF32);
    LOAD_AR_SYM(GetObject,         NvAR_GetObject);
    LOAD_AR_SYM(GetString,         NvAR_GetString);
    LOAD_AR_SYM(CudaStreamCreate,  NvAR_CudaStreamCreate);
    LOAD_AR_SYM(CudaStreamDestroy, NvAR_CudaStreamDestroy);

    /* Load NvCVImage function pointers */
    LOAD_CV_SYM(ImageInit,     NvCVImage_Init);
    LOAD_CV_SYM(ImageAlloc,    NvCVImage_Alloc);
    LOAD_CV_SYM(ImageRealloc,  NvCVImage_Realloc);
    LOAD_CV_SYM(ImageDealloc,  NvCVImage_Dealloc);
    LOAD_CV_SYM(ImageTransfer, NvCVImage_Transfer);
    LOAD_CV_SYM(GetErrorString, NvCV_GetErrorStringFromCode);

    g_sdk.loaded = true;
    return NVAR_OK;
}

#undef LOAD_AR_SYM
#undef LOAD_CV_SYM

/* ========================================================================== */
/*  Internal helper: forward SDK error with detail string                      */
/* ========================================================================== */

static int sdk_error(NvCV_Status status, const char* context) {
    const char* desc = g_sdk.GetErrorString ? g_sdk.GetErrorString(status) : "unknown";
    set_error("%s: SDK error %d (%s)", context, (int)status, desc);
    if (status <= NVCV_ERR_CUDA_BASE)
        return NVAR_ERR_CUDA;
    return NVAR_ERR_SDK;
}

/* ========================================================================== */
/*  Internal structures                                                        */
/* ========================================================================== */

struct NvarSession {
    CUstream stream;
    NvCVImage src_gpu;       /* GPU buffer (BGR u8, shared across features) */
    NvCVImage tmp;           /* Temp buffer for CPU->GPU transfer */
    char model_dir[1024];
    unsigned int frame_width;
    unsigned int frame_height;
    bool has_frame;
};

struct NvarFeature {
    NvarSession* session;
    NvAR_FeatureHandle handle;
    char feature_id[64];
    bool loaded;

    /* Queryable sizes (set after nvar_load) */
    unsigned int expr_count;
    unsigned int eigen_count;
    unsigned int vertex_count;
    unsigned int triangle_count;
    unsigned int landmark_count;
    unsigned int num_keypoints;

    /* Output buffers (allocated after nvar_load based on feature type) */
    std::vector<float> expressions;
    std::vector<float> eigenvalues;

    NvAR_FaceMesh mesh;
    std::vector<NvAR_Vector3f> vertices;
    std::vector<NvAR_Vector3u16> triangles;

    std::vector<NvAR_Point2f> landmarks;
    std::vector<NvAR_Point3f> landmarks_3d;
    std::vector<float> landmark_confidence;

    NvAR_Quaternion rotation;
    NvAR_Vector3f pose_translation;
    NvAR_RenderingParams render_params;

    std::vector<NvAR_Rect> bbox_data;
    NvAR_BBoxes bboxes;
    std::vector<float> bbox_confidence;

    float camera_intrinsics[3];

    /* Body outputs */
    std::vector<NvAR_Point2f> body_keypoints;
    std::vector<NvAR_Point3f> body_keypoints_3d;
    std::vector<float> body_joint_angles;
    std::vector<float> body_keypoint_confidence;
    std::vector<NvAR_TrackingBBox> tracking_bbox_data;
    NvAR_TrackingBBoxes tracking_bboxes;

    /* Gaze outputs */
    float gaze_angles[2];
    NvAR_Quaternion gaze_head_pose;
    float gaze_head_translation[3];
    NvAR_Point3f gaze_direction[2]; /* origin + direction */
};

/* ========================================================================== */
/*  Helpers: feature ID classification                                         */
/* ========================================================================== */

static bool is_face_feature(const char* id) {
    return strcmp(id, NvAR_Feature_FaceBoxDetection) == 0 ||
           strcmp(id, NvAR_Feature_LandmarkDetection) == 0 ||
           strcmp(id, NvAR_Feature_Face3DReconstruction) == 0 ||
           strcmp(id, NvAR_Feature_FaceExpressions) == 0;
}

static bool is_body_feature(const char* id) {
    return strcmp(id, NvAR_Feature_BodyDetection) == 0 ||
           strcmp(id, NvAR_Feature_BodyPoseEstimation) == 0;
}

static bool is_gaze_feature(const char* id) {
    return strcmp(id, NvAR_Feature_GazeRedirection) == 0;
}

static bool has_expressions(const char* id) {
    return strcmp(id, NvAR_Feature_FaceExpressions) == 0 ||
           strcmp(id, NvAR_Feature_Face3DReconstruction) == 0;
}

static bool has_mesh(const char* id) {
    return strcmp(id, NvAR_Feature_Face3DReconstruction) == 0;
}

static bool has_landmarks(const char* id) {
    return strcmp(id, NvAR_Feature_LandmarkDetection) == 0 ||
           strcmp(id, NvAR_Feature_Face3DReconstruction) == 0 ||
           strcmp(id, NvAR_Feature_FaceExpressions) == 0 ||
           strcmp(id, NvAR_Feature_GazeRedirection) == 0;
}

static bool has_pose(const char* id) {
    return strcmp(id, NvAR_Feature_LandmarkDetection) == 0 ||
           strcmp(id, NvAR_Feature_Face3DReconstruction) == 0 ||
           strcmp(id, NvAR_Feature_FaceExpressions) == 0;
}

static bool has_bboxes(const char* id) {
    return true; /* All features can produce bounding boxes */
}

/* ========================================================================== */
/*  NvCVImage pixel format mapping                                             */
/* ========================================================================== */

static NvCVImage_PixelFormat to_nvcv_format(int format) {
    switch (format) {
        case NVAR_FORMAT_RGB:  return NVCV_RGB;
        case NVAR_FORMAT_BGR:  return NVCV_BGR;
        case NVAR_FORMAT_RGBA: return NVCV_RGBA;
        default:               return NVCV_FORMAT_UNKNOWN;
    }
}

static unsigned int pixel_bytes(int format) {
    switch (format) {
        case NVAR_FORMAT_RGB:  return 3;
        case NVAR_FORMAT_BGR:  return 3;
        case NVAR_FORMAT_RGBA: return 4;
        default:               return 0;
    }
}

/* ========================================================================== */
/*  Session management                                                         */
/* ========================================================================== */

int NVAR_API nvar_create_session(NvarSession** out_session,
                                 const char* model_dir,
                                 int cuda_device) {
    clear_error();

    if (!out_session) {
        set_error("out_session is NULL");
        return NVAR_ERR_INVALID_ARG;
    }
    *out_session = nullptr;

    int rc = ensure_sdk_loaded();
    if (rc != NVAR_OK) return rc;

    NvarSession* session = new (std::nothrow) NvarSession{};
    if (!session) {
        set_error("Failed to allocate NvarSession");
        return NVAR_ERR_CUDA;
    }

    /* Store model directory */
    if (model_dir) {
        strncpy(session->model_dir, model_dir, sizeof(session->model_dir) - 1);
        session->model_dir[sizeof(session->model_dir) - 1] = '\0';
    }

    /* Create CUDA stream via the SDK wrapper (avoids direct CUDA linkage) */
    NvCV_Status status = g_sdk.CudaStreamCreate(&session->stream);
    if (status != NVCV_SUCCESS) {
        delete session;
        return sdk_error(status, "nvar_create_session: CudaStreamCreate");
    }

    *out_session = session;
    return NVAR_OK;
}

int NVAR_API nvar_destroy_session(NvarSession* session) {
    clear_error();
    if (!session) return NVAR_OK;

    /* Deallocate GPU image buffers */
    if (session->src_gpu.pixels) g_sdk.ImageDealloc(&session->src_gpu);
    if (session->tmp.pixels)     g_sdk.ImageDealloc(&session->tmp);

    /* Destroy CUDA stream */
    if (session->stream) g_sdk.CudaStreamDestroy(session->stream);

    delete session;
    return NVAR_OK;
}

int NVAR_API nvar_push_frame(NvarSession* session,
                              const void* pixels,
                              unsigned int width,
                              unsigned int height,
                              int format) {
    clear_error();

    if (!session) {
        set_error("session is NULL");
        return NVAR_ERR_NULL_HANDLE;
    }
    if (!pixels) {
        set_error("pixels is NULL");
        return NVAR_ERR_INVALID_ARG;
    }
    if (width == 0 || height == 0) {
        set_error("width and height must be > 0");
        return NVAR_ERR_INVALID_ARG;
    }

    NvCVImage_PixelFormat nvcv_fmt = to_nvcv_format(format);
    if (nvcv_fmt == NVCV_FORMAT_UNKNOWN) {
        set_error("Invalid pixel format: %d", format);
        return NVAR_ERR_INVALID_ARG;
    }

    /* The SDK expects BGR u8 input on GPU. We'll transfer from whatever the
     * caller gives us — NvCVImage_Transfer handles format conversion. */

    /* (Re)allocate GPU buffer if dimensions changed */
    if (session->frame_width != width || session->frame_height != height ||
        session->src_gpu.pixels == nullptr) {

        NvCV_Status status = g_sdk.ImageRealloc(&session->src_gpu,
            width, height, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 0);
        if (status != NVCV_SUCCESS)
            return sdk_error(status, "nvar_push_frame: GPU buffer realloc");

        session->frame_width = width;
        session->frame_height = height;
    }

    /* Wrap the caller's pixel buffer as a zero-copy CPU NvCVImage */
    NvCVImage src_wrapper;
    memset(&src_wrapper, 0, sizeof(src_wrapper));
    unsigned int pbytes = pixel_bytes(format);
    NvCV_Status status = g_sdk.ImageInit(&src_wrapper, width, height,
        (int)(width * pbytes), const_cast<void*>(pixels),
        nvcv_fmt, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
    if (status != NVCV_SUCCESS)
        return sdk_error(status, "nvar_push_frame: ImageInit");

    /* Transfer CPU -> GPU (handles format conversion, e.g. RGB->BGR) */
    status = g_sdk.ImageTransfer(&src_wrapper, &session->src_gpu, 1.0f,
        session->stream, &session->tmp);
    if (status != NVCV_SUCCESS)
        return sdk_error(status, "nvar_push_frame: ImageTransfer");

    session->has_frame = true;
    return NVAR_OK;
}

/* ========================================================================== */
/*  Feature management                                                         */
/* ========================================================================== */

int NVAR_API nvar_create_feature(NvarSession* session,
                                  const char* feature_id,
                                  NvarFeature** out_feature) {
    clear_error();

    if (!session) {
        set_error("session is NULL");
        return NVAR_ERR_NULL_HANDLE;
    }
    if (!feature_id || !feature_id[0]) {
        set_error("feature_id is NULL or empty");
        return NVAR_ERR_INVALID_ARG;
    }
    if (!out_feature) {
        set_error("out_feature is NULL");
        return NVAR_ERR_INVALID_ARG;
    }
    *out_feature = nullptr;

    NvarFeature* feat = new (std::nothrow) NvarFeature{};
    if (!feat) {
        set_error("Failed to allocate NvarFeature");
        return NVAR_ERR_CUDA;
    }

    feat->session = session;
    strncpy(feat->feature_id, feature_id, sizeof(feat->feature_id) - 1);
    feat->feature_id[sizeof(feat->feature_id) - 1] = '\0';

    NvCV_Status status = g_sdk.Create(feature_id, &feat->handle);
    if (status != NVCV_SUCCESS) {
        delete feat;
        return sdk_error(status, "nvar_create_feature: NvAR_Create");
    }

    *out_feature = feat;
    return NVAR_OK;
}

int NVAR_API nvar_destroy_feature(NvarFeature* feature) {
    clear_error();
    if (!feature) return NVAR_OK;

    if (feature->handle)
        g_sdk.Destroy(feature->handle);

    delete feature;
    return NVAR_OK;
}

int NVAR_API nvar_set_u32(NvarFeature* feature, const char* param_name, unsigned int value) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!param_name) { set_error("param_name is NULL"); return NVAR_ERR_INVALID_ARG; }

    NvCV_Status status = g_sdk.SetU32(feature->handle, param_name, value);
    if (status != NVCV_SUCCESS)
        return sdk_error(status, "nvar_set_u32");
    return NVAR_OK;
}

int NVAR_API nvar_set_string(NvarFeature* feature, const char* param_name, const char* value) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!param_name) { set_error("param_name is NULL"); return NVAR_ERR_INVALID_ARG; }

    NvCV_Status status = g_sdk.SetString(feature->handle, param_name, value);
    if (status != NVCV_SUCCESS)
        return sdk_error(status, "nvar_set_string");
    return NVAR_OK;
}

int NVAR_API nvar_set_f32(NvarFeature* feature, const char* param_name, float value) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!param_name) { set_error("param_name is NULL"); return NVAR_ERR_INVALID_ARG; }

    NvCV_Status status = g_sdk.SetF32(feature->handle, param_name, value);
    if (status != NVCV_SUCCESS)
        return sdk_error(status, "nvar_set_f32");
    return NVAR_OK;
}

/* -------------------------------------------------------------------------- */
/*  nvar_load — Load model, query sizes, allocate & bind output buffers        */
/* -------------------------------------------------------------------------- */

int NVAR_API nvar_load(NvarFeature* feature) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!feature->session) { set_error("feature has no session"); return NVAR_ERR_NULL_HANDLE; }

    NvAR_FeatureHandle h = feature->handle;
    NvarSession* s = feature->session;
    NvCV_Status status;
    const char* id = feature->feature_id;

    /* Set model directory */
    if (s->model_dir[0]) {
        /* Face3DReconstruction and FaceExpressions use "ModelDir",
         * detection features use "TRTModelDir" */
        if (strcmp(id, NvAR_Feature_Face3DReconstruction) == 0 ||
            strcmp(id, NvAR_Feature_FaceExpressions) == 0 ||
            strcmp(id, NvAR_Feature_LandmarkDetection) == 0 ||
            strcmp(id, NvAR_Feature_BodyPoseEstimation) == 0 ||
            strcmp(id, NvAR_Feature_GazeRedirection) == 0) {
            g_sdk.SetString(h, NvAR_Parameter_Config(ModelDir), s->model_dir);
        } else {
            g_sdk.SetString(h, NvAR_Parameter_Config(TRTModelDir), s->model_dir);
        }
    }

    /* Set CUDA stream */
    g_sdk.SetCudaStream(h, NvAR_Parameter_Config(CUDAStream), s->stream);

    /* Load the model */
    status = g_sdk.Load(h);
    if (status != NVCV_SUCCESS)
        return sdk_error(status, "nvar_load: NvAR_Load");

    /* --- Query sizes and allocate output buffers --- */

    /* Expression coefficients (FaceExpressions, Face3DReconstruction) */
    if (has_expressions(id)) {
        unsigned int count = 0;
        status = g_sdk.GetU32(h, NvAR_Parameter_Config(ExpressionCount), &count);
        if (status == NVCV_SUCCESS && count > 0) {
            feature->expr_count = count;
            feature->expressions.resize(count, 0.0f);
            g_sdk.SetF32Array(h, NvAR_Parameter_Output(ExpressionCoefficients),
                feature->expressions.data(), (int)count);
        }
    }

    /* Face mesh (Face3DReconstruction) */
    if (has_mesh(id)) {
        unsigned int vc = 0, tc = 0, ec = 0;
        g_sdk.GetU32(h, NvAR_Parameter_Config(VertexCount), &vc);
        g_sdk.GetU32(h, NvAR_Parameter_Config(TriangleCount), &tc);
        g_sdk.GetU32(h, NvAR_Parameter_Config(ShapeEigenValueCount), &ec);

        if (vc > 0) {
            feature->vertex_count = vc;
            feature->vertices.resize(vc);
            feature->mesh.vertices = feature->vertices.data();
            feature->mesh.num_vertices = vc;
        }
        if (tc > 0) {
            feature->triangle_count = tc;
            feature->triangles.resize(tc);
            feature->mesh.tvi = feature->triangles.data();
            feature->mesh.num_triangles = tc;
        }
        if (vc > 0 || tc > 0) {
            g_sdk.SetObject(h, NvAR_Parameter_Output(FaceMesh),
                &feature->mesh, sizeof(NvAR_FaceMesh));
        }

        /* Rendering params (includes rotation + translation) */
        g_sdk.SetObject(h, NvAR_Parameter_Output(RenderingParams),
            &feature->render_params, sizeof(NvAR_RenderingParams));

        /* Eigenvalues */
        if (ec > 0) {
            feature->eigen_count = ec;
            feature->eigenvalues.resize(ec, 0.0f);
            g_sdk.SetF32Array(h, NvAR_Parameter_Output(ShapeEigenValues),
                feature->eigenvalues.data(), (int)ec);
        }
    }

    /* Landmarks (multiple features) */
    if (has_landmarks(id)) {
        unsigned int lm_size = 0;
        g_sdk.GetU32(h, NvAR_Parameter_Config(Landmarks_Size), &lm_size);
        if (lm_size > 0) {
            feature->landmark_count = lm_size;
            feature->landmarks.resize(lm_size);
            g_sdk.SetObject(h, NvAR_Parameter_Output(Landmarks),
                feature->landmarks.data(), (unsigned long)(lm_size * sizeof(NvAR_Point2f)));

            feature->landmark_confidence.resize(lm_size, 0.0f);
            g_sdk.SetF32Array(h, NvAR_Parameter_Output(LandmarksConfidence),
                feature->landmark_confidence.data(), (int)lm_size);

            /* Note: Face3DReconstruction outputs 3D vertex positions via FaceMesh,
             * not as separate 3D landmarks. Use nvar_get_mesh_vertices() instead. */
        }
    }

    /* Pose (rotation + translation) */
    if (has_pose(id)) {
        g_sdk.SetObject(h, NvAR_Parameter_Output(Pose),
            &feature->rotation, sizeof(NvAR_Quaternion));

        /* Face3DReconstruction gets translation from RenderingParams (bound above).
         * LandmarkDetection and FaceExpressions use a separate PoseTranslation output. */
        if (strcmp(id, NvAR_Feature_Face3DReconstruction) != 0) {
            g_sdk.SetObject(h, NvAR_Parameter_Output(PoseTranslation),
                &feature->pose_translation, sizeof(NvAR_Vector3f));
        }
    }

    /* Bounding boxes */
    if (has_bboxes(id)) {
        unsigned int max_faces = 1;
        if (is_face_feature(id)) max_faces = 8;  /* reasonable max */
        if (is_body_feature(id)) max_faces = 16;

        feature->bbox_data.resize(max_faces);
        feature->bboxes.boxes = feature->bbox_data.data();
        feature->bboxes.max_boxes = (uint8_t)(max_faces > 255 ? 255 : max_faces);
        feature->bboxes.num_boxes = 0;
        g_sdk.SetObject(h, NvAR_Parameter_Output(BoundingBoxes),
            &feature->bboxes, sizeof(NvAR_BBoxes));

        feature->bbox_confidence.resize(max_faces, 0.0f);
        g_sdk.SetF32Array(h, NvAR_Parameter_Output(BoundingBoxesConfidence),
            feature->bbox_confidence.data(), (int)max_faces);
    }

    /* Body keypoints (BodyPoseEstimation) */
    if (strcmp(id, NvAR_Feature_BodyPoseEstimation) == 0) {
        unsigned int nk = 0;
        g_sdk.GetU32(h, NvAR_Parameter_Config(NumKeyPoints), &nk);
        if (nk > 0) {
            feature->num_keypoints = nk;

            feature->body_keypoints.resize(nk);
            g_sdk.SetObject(h, NvAR_Parameter_Output(KeyPoints),
                feature->body_keypoints.data(), (unsigned long)(nk * sizeof(NvAR_Point2f)));

            feature->body_keypoints_3d.resize(nk);
            g_sdk.SetObject(h, NvAR_Parameter_Output(KeyPoints3D),
                feature->body_keypoints_3d.data(), (unsigned long)(nk * sizeof(NvAR_Point3f)));

            feature->body_joint_angles.resize(nk, 0.0f);
            g_sdk.SetF32Array(h, NvAR_Parameter_Output(JointAngles),
                feature->body_joint_angles.data(), (int)nk);

            feature->body_keypoint_confidence.resize(nk, 0.0f);
            g_sdk.SetF32Array(h, NvAR_Parameter_Output(KeyPointsConfidence),
                feature->body_keypoint_confidence.data(), (int)nk);
        }

        /* Tracking bounding boxes */
        unsigned int max_tracked = 16;
        feature->tracking_bbox_data.resize(max_tracked);
        feature->tracking_bboxes.boxes = feature->tracking_bbox_data.data();
        feature->tracking_bboxes.max_boxes = (uint8_t)(max_tracked > 255 ? 255 : max_tracked);
        feature->tracking_bboxes.num_boxes = 0;
        g_sdk.SetObject(h, NvAR_Parameter_Output(TrackingBoundingBoxes),
            &feature->tracking_bboxes, sizeof(NvAR_TrackingBBoxes));
    }

    /* Gaze outputs (GazeRedirection) */
    if (is_gaze_feature(id)) {
        g_sdk.SetObject(h, NvAR_Parameter_Output(OutputGazeVector),
            feature->gaze_direction, sizeof(feature->gaze_direction));
        g_sdk.SetObject(h, NvAR_Parameter_Output(HeadPose),
            &feature->gaze_head_pose, sizeof(NvAR_Quaternion));
        g_sdk.SetObject(h, NvAR_Parameter_Output(OutputHeadTranslation),
            feature->gaze_head_translation, sizeof(feature->gaze_head_translation));
    }

    /* Bind the shared GPU input image if a frame has been pushed */
    if (s->has_frame) {
        g_sdk.SetObject(h, NvAR_Parameter_Input(Image),
            &s->src_gpu, sizeof(NvCVImage));
    }

    feature->loaded = true;
    return NVAR_OK;
}

int NVAR_API nvar_run(NvarFeature* feature) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!feature->loaded) { set_error("feature not loaded"); return NVAR_ERR_NOT_LOADED; }
    if (!feature->session || !feature->session->has_frame) {
        set_error("no frame pushed");
        return NVAR_ERR_NO_FRAME;
    }

    /* Re-bind input image (in case frame dimensions changed) */
    g_sdk.SetObject(feature->handle, NvAR_Parameter_Input(Image),
        &feature->session->src_gpu, sizeof(NvCVImage));

    NvCV_Status status = g_sdk.Run(feature->handle);
    if (status != NVCV_SUCCESS)
        return sdk_error(status, "nvar_run");

    return NVAR_OK;
}

int NVAR_API nvar_get_u32(NvarFeature* feature, const char* param_name, unsigned int* out_value) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!param_name) { set_error("param_name is NULL"); return NVAR_ERR_INVALID_ARG; }
    if (!out_value) { set_error("out_value is NULL"); return NVAR_ERR_INVALID_ARG; }

    NvCV_Status status = g_sdk.GetU32(feature->handle, param_name, out_value);
    if (status != NVCV_SUCCESS)
        return sdk_error(status, "nvar_get_u32");
    return NVAR_OK;
}

/* ========================================================================== */
/*  Output getters                                                             */
/* ========================================================================== */

int NVAR_API nvar_get_expressions(NvarFeature* feature, float* out, unsigned int* count) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!out || !count) { set_error("out or count is NULL"); return NVAR_ERR_INVALID_ARG; }
    if (!feature->loaded) { set_error("feature not loaded"); return NVAR_ERR_NOT_LOADED; }
    if (feature->expr_count == 0) { set_error("feature has no expressions"); return NVAR_ERR_NOT_AVAILABLE; }

    if (*count < feature->expr_count) {
        set_error("buffer too small: need %u, got %u", feature->expr_count, *count);
        *count = feature->expr_count;
        return NVAR_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(out, feature->expressions.data(), feature->expr_count * sizeof(float));
    *count = feature->expr_count;
    return NVAR_OK;
}

int NVAR_API nvar_get_eigenvalues(NvarFeature* feature, float* out, unsigned int* count) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!out || !count) { set_error("out or count is NULL"); return NVAR_ERR_INVALID_ARG; }
    if (!feature->loaded) { set_error("feature not loaded"); return NVAR_ERR_NOT_LOADED; }
    if (feature->eigen_count == 0) { set_error("feature has no eigenvalues"); return NVAR_ERR_NOT_AVAILABLE; }

    if (*count < feature->eigen_count) {
        set_error("buffer too small: need %u, got %u", feature->eigen_count, *count);
        *count = feature->eigen_count;
        return NVAR_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(out, feature->eigenvalues.data(), feature->eigen_count * sizeof(float));
    *count = feature->eigen_count;
    return NVAR_OK;
}

int NVAR_API nvar_get_mesh_vertices(NvarFeature* feature, float* out, unsigned int* vertex_count) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!out || !vertex_count) { set_error("out or vertex_count is NULL"); return NVAR_ERR_INVALID_ARG; }
    if (!feature->loaded) { set_error("feature not loaded"); return NVAR_ERR_NOT_LOADED; }
    if (feature->vertex_count == 0) { set_error("feature has no mesh"); return NVAR_ERR_NOT_AVAILABLE; }

    if (*vertex_count < feature->vertex_count) {
        set_error("buffer too small: need %u vertices, got %u", feature->vertex_count, *vertex_count);
        *vertex_count = feature->vertex_count;
        return NVAR_ERR_BUFFER_TOO_SMALL;
    }

    /* Copy NvAR_Vector3f[] to flat [x,y,z, x,y,z, ...] */
    for (unsigned int i = 0; i < feature->vertex_count; i++) {
        out[i * 3 + 0] = feature->vertices[i].vec[0];
        out[i * 3 + 1] = feature->vertices[i].vec[1];
        out[i * 3 + 2] = feature->vertices[i].vec[2];
    }
    *vertex_count = feature->vertex_count;
    return NVAR_OK;
}

int NVAR_API nvar_get_mesh_triangles(NvarFeature* feature, unsigned short* out, unsigned int* triangle_count) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!out || !triangle_count) { set_error("out or triangle_count is NULL"); return NVAR_ERR_INVALID_ARG; }
    if (!feature->loaded) { set_error("feature not loaded"); return NVAR_ERR_NOT_LOADED; }
    if (feature->triangle_count == 0) { set_error("feature has no mesh"); return NVAR_ERR_NOT_AVAILABLE; }

    if (*triangle_count < feature->triangle_count) {
        set_error("buffer too small: need %u triangles, got %u", feature->triangle_count, *triangle_count);
        *triangle_count = feature->triangle_count;
        return NVAR_ERR_BUFFER_TOO_SMALL;
    }

    /* Copy NvAR_Vector3u16[] to flat [v0,v1,v2, v0,v1,v2, ...] */
    for (unsigned int i = 0; i < feature->triangle_count; i++) {
        out[i * 3 + 0] = feature->triangles[i].vec[0];
        out[i * 3 + 1] = feature->triangles[i].vec[1];
        out[i * 3 + 2] = feature->triangles[i].vec[2];
    }
    *triangle_count = feature->triangle_count;
    return NVAR_OK;
}

int NVAR_API nvar_get_landmarks_2d(NvarFeature* feature, float* out, unsigned int* count) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!out || !count) { set_error("out or count is NULL"); return NVAR_ERR_INVALID_ARG; }
    if (!feature->loaded) { set_error("feature not loaded"); return NVAR_ERR_NOT_LOADED; }

    unsigned int actual = feature->landmark_count;
    if (is_body_feature(feature->feature_id))
        actual = feature->num_keypoints;

    if (actual == 0) { set_error("feature has no 2D landmarks"); return NVAR_ERR_NOT_AVAILABLE; }

    if (*count < actual) {
        set_error("buffer too small: need %u, got %u", actual, *count);
        *count = actual;
        return NVAR_ERR_BUFFER_TOO_SMALL;
    }

    /* Copy NvAR_Point2f[] to flat [x,y, x,y, ...] */
    const NvAR_Point2f* src = is_body_feature(feature->feature_id)
        ? feature->body_keypoints.data()
        : feature->landmarks.data();
    for (unsigned int i = 0; i < actual; i++) {
        out[i * 2 + 0] = src[i].x;
        out[i * 2 + 1] = src[i].y;
    }
    *count = actual;
    return NVAR_OK;
}

int NVAR_API nvar_get_landmarks_3d(NvarFeature* feature, float* out, unsigned int* count) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!out || !count) { set_error("out or count is NULL"); return NVAR_ERR_INVALID_ARG; }
    if (!feature->loaded) { set_error("feature not loaded"); return NVAR_ERR_NOT_LOADED; }

    /* 3D landmarks from Face3DReconstruction */
    if (!feature->landmarks_3d.empty()) {
        unsigned int actual = (unsigned int)feature->landmarks_3d.size();
        if (*count < actual) {
            set_error("buffer too small: need %u, got %u", actual, *count);
            *count = actual;
            return NVAR_ERR_BUFFER_TOO_SMALL;
        }
        for (unsigned int i = 0; i < actual; i++) {
            out[i * 3 + 0] = feature->landmarks_3d[i].x;
            out[i * 3 + 1] = feature->landmarks_3d[i].y;
            out[i * 3 + 2] = feature->landmarks_3d[i].z;
        }
        *count = actual;
        return NVAR_OK;
    }

    /* 3D keypoints from BodyPoseEstimation */
    if (!feature->body_keypoints_3d.empty()) {
        unsigned int actual = (unsigned int)feature->body_keypoints_3d.size();
        if (*count < actual) {
            set_error("buffer too small: need %u, got %u", actual, *count);
            *count = actual;
            return NVAR_ERR_BUFFER_TOO_SMALL;
        }
        for (unsigned int i = 0; i < actual; i++) {
            out[i * 3 + 0] = feature->body_keypoints_3d[i].x;
            out[i * 3 + 1] = feature->body_keypoints_3d[i].y;
            out[i * 3 + 2] = feature->body_keypoints_3d[i].z;
        }
        *count = actual;
        return NVAR_OK;
    }

    set_error("feature has no 3D landmarks/keypoints");
    return NVAR_ERR_NOT_AVAILABLE;
}

int NVAR_API nvar_get_landmarks_confidence(NvarFeature* feature, float* out, unsigned int* count) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!out || !count) { set_error("out or count is NULL"); return NVAR_ERR_INVALID_ARG; }
    if (!feature->loaded) { set_error("feature not loaded"); return NVAR_ERR_NOT_LOADED; }

    const float* src = nullptr;
    unsigned int actual = 0;

    if (is_body_feature(feature->feature_id) && !feature->body_keypoint_confidence.empty()) {
        src = feature->body_keypoint_confidence.data();
        actual = (unsigned int)feature->body_keypoint_confidence.size();
    } else if (!feature->landmark_confidence.empty()) {
        src = feature->landmark_confidence.data();
        actual = (unsigned int)feature->landmark_confidence.size();
    }

    if (!src || actual == 0) {
        set_error("feature has no landmark confidence");
        return NVAR_ERR_NOT_AVAILABLE;
    }

    if (*count < actual) {
        set_error("buffer too small: need %u, got %u", actual, *count);
        *count = actual;
        return NVAR_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(out, src, actual * sizeof(float));
    *count = actual;
    return NVAR_OK;
}

int NVAR_API nvar_get_pose(NvarFeature* feature, float* out_rotation_xyzw, float* out_translation_xyz) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!out_rotation_xyzw || !out_translation_xyz) {
        set_error("output pointers are NULL");
        return NVAR_ERR_INVALID_ARG;
    }
    if (!feature->loaded) { set_error("feature not loaded"); return NVAR_ERR_NOT_LOADED; }
    if (!has_pose(feature->feature_id)) {
        set_error("feature does not produce pose");
        return NVAR_ERR_NOT_AVAILABLE;
    }

    /* For Face3DReconstruction, pose comes from RenderingParams */
    if (strcmp(feature->feature_id, NvAR_Feature_Face3DReconstruction) == 0) {
        out_rotation_xyzw[0] = feature->render_params.rotation.x;
        out_rotation_xyzw[1] = feature->render_params.rotation.y;
        out_rotation_xyzw[2] = feature->render_params.rotation.z;
        out_rotation_xyzw[3] = feature->render_params.rotation.w;
        out_translation_xyz[0] = feature->render_params.translation.vec[0];
        out_translation_xyz[1] = feature->render_params.translation.vec[1];
        out_translation_xyz[2] = feature->render_params.translation.vec[2];
    } else {
        out_rotation_xyzw[0] = feature->rotation.x;
        out_rotation_xyzw[1] = feature->rotation.y;
        out_rotation_xyzw[2] = feature->rotation.z;
        out_rotation_xyzw[3] = feature->rotation.w;
        out_translation_xyz[0] = feature->pose_translation.vec[0];
        out_translation_xyz[1] = feature->pose_translation.vec[1];
        out_translation_xyz[2] = feature->pose_translation.vec[2];
    }

    return NVAR_OK;
}

int NVAR_API nvar_get_bounding_boxes(NvarFeature* feature, float* out, unsigned int* count) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!out || !count) { set_error("out or count is NULL"); return NVAR_ERR_INVALID_ARG; }
    if (!feature->loaded) { set_error("feature not loaded"); return NVAR_ERR_NOT_LOADED; }

    unsigned int actual = feature->bboxes.num_boxes;
    if (actual == 0) {
        *count = 0;
        return NVAR_OK;
    }

    if (*count < actual) {
        set_error("buffer too small: need %u, got %u", actual, *count);
        *count = actual;
        return NVAR_ERR_BUFFER_TOO_SMALL;
    }

    /* Copy NvAR_Rect[] to flat [x,y,w,h, x,y,w,h, ...] */
    for (unsigned int i = 0; i < actual; i++) {
        out[i * 4 + 0] = feature->bbox_data[i].x;
        out[i * 4 + 1] = feature->bbox_data[i].y;
        out[i * 4 + 2] = feature->bbox_data[i].width;
        out[i * 4 + 3] = feature->bbox_data[i].height;
    }
    *count = actual;
    return NVAR_OK;
}

int NVAR_API nvar_get_body_keypoints_2d(NvarFeature* feature, float* out, unsigned int* count) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!out || !count) { set_error("out or count is NULL"); return NVAR_ERR_INVALID_ARG; }
    if (!feature->loaded) { set_error("feature not loaded"); return NVAR_ERR_NOT_LOADED; }
    if (feature->body_keypoints.empty()) {
        set_error("feature has no body keypoints");
        return NVAR_ERR_NOT_AVAILABLE;
    }

    unsigned int actual = (unsigned int)feature->body_keypoints.size();
    if (*count < actual) {
        set_error("buffer too small: need %u, got %u", actual, *count);
        *count = actual;
        return NVAR_ERR_BUFFER_TOO_SMALL;
    }

    for (unsigned int i = 0; i < actual; i++) {
        out[i * 2 + 0] = feature->body_keypoints[i].x;
        out[i * 2 + 1] = feature->body_keypoints[i].y;
    }
    *count = actual;
    return NVAR_OK;
}

int NVAR_API nvar_get_body_keypoints_3d(NvarFeature* feature, float* out, unsigned int* count) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!out || !count) { set_error("out or count is NULL"); return NVAR_ERR_INVALID_ARG; }
    if (!feature->loaded) { set_error("feature not loaded"); return NVAR_ERR_NOT_LOADED; }
    if (feature->body_keypoints_3d.empty()) {
        set_error("feature has no body 3D keypoints");
        return NVAR_ERR_NOT_AVAILABLE;
    }

    unsigned int actual = (unsigned int)feature->body_keypoints_3d.size();
    if (*count < actual) {
        set_error("buffer too small: need %u, got %u", actual, *count);
        *count = actual;
        return NVAR_ERR_BUFFER_TOO_SMALL;
    }

    for (unsigned int i = 0; i < actual; i++) {
        out[i * 3 + 0] = feature->body_keypoints_3d[i].x;
        out[i * 3 + 1] = feature->body_keypoints_3d[i].y;
        out[i * 3 + 2] = feature->body_keypoints_3d[i].z;
    }
    *count = actual;
    return NVAR_OK;
}

int NVAR_API nvar_get_body_joint_angles(NvarFeature* feature, float* out, unsigned int* count) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!out || !count) { set_error("out or count is NULL"); return NVAR_ERR_INVALID_ARG; }
    if (!feature->loaded) { set_error("feature not loaded"); return NVAR_ERR_NOT_LOADED; }
    if (feature->body_joint_angles.empty()) {
        set_error("feature has no joint angles");
        return NVAR_ERR_NOT_AVAILABLE;
    }

    unsigned int actual = (unsigned int)feature->body_joint_angles.size();
    if (*count < actual) {
        set_error("buffer too small: need %u, got %u", actual, *count);
        *count = actual;
        return NVAR_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(out, feature->body_joint_angles.data(), actual * sizeof(float));
    *count = actual;
    return NVAR_OK;
}

int NVAR_API nvar_get_tracking_bboxes(NvarFeature* feature, float* out, unsigned int* count) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!out || !count) { set_error("out or count is NULL"); return NVAR_ERR_INVALID_ARG; }
    if (!feature->loaded) { set_error("feature not loaded"); return NVAR_ERR_NOT_LOADED; }

    unsigned int actual = feature->tracking_bboxes.num_boxes;
    if (actual == 0) {
        *count = 0;
        return NVAR_OK;
    }

    if (*count < actual) {
        set_error("buffer too small: need %u, got %u", actual, *count);
        *count = actual;
        return NVAR_ERR_BUFFER_TOO_SMALL;
    }

    /* Copy to flat [x,y,w,h,tracking_id, ...] — 5 floats per entry */
    for (unsigned int i = 0; i < actual; i++) {
        out[i * 5 + 0] = feature->tracking_bbox_data[i].bbox.x;
        out[i * 5 + 1] = feature->tracking_bbox_data[i].bbox.y;
        out[i * 5 + 2] = feature->tracking_bbox_data[i].bbox.width;
        out[i * 5 + 3] = feature->tracking_bbox_data[i].bbox.height;
        out[i * 5 + 4] = (float)feature->tracking_bbox_data[i].tracking_id;
    }
    *count = actual;
    return NVAR_OK;
}

int NVAR_API nvar_get_gaze_angles(NvarFeature* feature, float* out) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!out) { set_error("out is NULL"); return NVAR_ERR_INVALID_ARG; }
    if (!feature->loaded) { set_error("feature not loaded"); return NVAR_ERR_NOT_LOADED; }
    if (!is_gaze_feature(feature->feature_id)) {
        set_error("feature does not produce gaze");
        return NVAR_ERR_NOT_AVAILABLE;
    }

    /* Gaze direction comes as two Point3f (origin + direction).
     * We convert to pitch/yaw for convenience. */
    float dx = feature->gaze_direction[1].x;
    float dy = feature->gaze_direction[1].y;
    float dz = feature->gaze_direction[1].z;

    /* pitch = atan2(dy, sqrt(dx^2 + dz^2)), yaw = atan2(dx, dz) */
    float len_xz = sqrtf(dx * dx + dz * dz);
    out[0] = atan2f(dy, len_xz);  /* pitch */
    out[1] = atan2f(dx, dz);      /* yaw */

    return NVAR_OK;
}

int NVAR_API nvar_get_gaze_head_pose(NvarFeature* feature, float* out_rotation_xyzw) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!out_rotation_xyzw) { set_error("out is NULL"); return NVAR_ERR_INVALID_ARG; }
    if (!feature->loaded) { set_error("feature not loaded"); return NVAR_ERR_NOT_LOADED; }
    if (!is_gaze_feature(feature->feature_id)) {
        set_error("feature does not produce gaze head pose");
        return NVAR_ERR_NOT_AVAILABLE;
    }

    out_rotation_xyzw[0] = feature->gaze_head_pose.x;
    out_rotation_xyzw[1] = feature->gaze_head_pose.y;
    out_rotation_xyzw[2] = feature->gaze_head_pose.z;
    out_rotation_xyzw[3] = feature->gaze_head_pose.w;
    return NVAR_OK;
}

int NVAR_API nvar_get_gaze_head_translation(NvarFeature* feature, float* out_xyz) {
    clear_error();
    if (!feature) { set_error("feature is NULL"); return NVAR_ERR_NULL_HANDLE; }
    if (!out_xyz) { set_error("out is NULL"); return NVAR_ERR_INVALID_ARG; }
    if (!feature->loaded) { set_error("feature not loaded"); return NVAR_ERR_NOT_LOADED; }
    if (!is_gaze_feature(feature->feature_id)) {
        set_error("feature does not produce gaze head translation");
        return NVAR_ERR_NOT_AVAILABLE;
    }

    out_xyz[0] = feature->gaze_head_translation[0];
    out_xyz[1] = feature->gaze_head_translation[1];
    out_xyz[2] = feature->gaze_head_translation[2];
    return NVAR_OK;
}

/* ========================================================================== */
/*  Error handling                                                             */
/* ========================================================================== */

const char* NVAR_API nvar_get_error_string(int result) {
    switch (result) {
        case NVAR_OK:                   return "success";
        case NVAR_ERR_INVALID_ARG:      return "invalid argument";
        case NVAR_ERR_NULL_HANDLE:      return "null handle";
        case NVAR_ERR_SDK:              return "SDK error";
        case NVAR_ERR_CUDA:             return "CUDA error";
        case NVAR_ERR_NOT_LOADED:       return "feature not loaded";
        case NVAR_ERR_NO_FRAME:         return "no frame pushed";
        case NVAR_ERR_BUFFER_TOO_SMALL: return "buffer too small";
        case NVAR_ERR_NOT_AVAILABLE:    return "output not available for this feature";
        case NVAR_ERR_LIBRARY:          return "SDK library not found";
        default:                        return "unknown error";
    }
}

const char* NVAR_API nvar_get_last_error(void) {
    return g_last_error;
}

/* ========================================================================== */
/*  SDK version                                                                */
/* ========================================================================== */

int NVAR_API nvar_get_sdk_version(unsigned int* out_version) {
    clear_error();
    if (!out_version) { set_error("out_version is NULL"); return NVAR_ERR_INVALID_ARG; }

    int rc = ensure_sdk_loaded();
    if (rc != NVAR_OK) return rc;

    NvCV_Status status = g_sdk.GetVersion(out_version);
    if (status != NVCV_SUCCESS)
        return sdk_error(status, "nvar_get_sdk_version");

    return NVAR_OK;
}
