#pragma once

#include <android/log.h>

// 默认日志标签，可在包含此头文件前通过 #define LOG_TAG 自定义
#ifndef LOG_TAG
#define LOG_TAG "LSPlant"
#endif

// 当定义了 LOG_DISABLED 时，所有日志宏都替换为空操作（返回 0），用于发布构建中完全禁用日志
#ifdef LOG_DISABLED
#define LOGD(...) 0
#define LOGV(...) 0
#define LOGI(...) 0
#define LOGW(...) 0
#define LOGE(...) 0
#define PLOGE(...) 0
#else
// Debug 级别日志，仅在非 NDEBUG 构建中启用，自动附加文件名和行号信息
#ifndef NDEBUG
#define LOGD(fmt, ...)                                                                             \
    __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG,                                                \
                        "%s:%d"                                                                    \
                        ": " fmt,                                                                  \
                        __FILE_NAME__, __LINE__ __VA_OPT__(, ) __VA_ARGS__)
// Verbose 级别日志，仅在非 NDEBUG 构建中启用，自动附加文件名和行号信息
#define LOGV(fmt, ...)                                                                             \
    __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG,                                              \
                        "%s:%d"                                                                    \
                        ": " fmt,                                                                  \
                        __FILE_NAME__, __LINE__ __VA_OPT__(, ) __VA_ARGS__)
// Debug 和 Verbose 级别日志在 Release 构建中被禁用，替换为空操作
#else
#define LOGD(...) 0
#define LOGV(...) 0
#endif
// Info 级别日志，始终启用，用于记录一般性信息
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
// Warning 级别日志，始终启用，用于记录警告信息
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
// Error 级别日志，始终启用，用于记录错误信息
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
// Fatal 级别日志，用于记录致命错误
#define LOGF(...) __android_log_print(ANDROID_LOG_FATAL, LOG_TAG, __VA_ARGS__)
// 带错误码的 Error 日志，自动追加 errno 和对应的错误描述字符串
#define PLOGE(fmt, args...) LOGE(fmt " failed with %d: %s", ##args, errno, strerror(errno))
#endif
