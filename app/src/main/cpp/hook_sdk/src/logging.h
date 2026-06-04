#pragma once

#include <android/log.h>

#define HOOK_LOG_TAG "HookSDK"

#ifdef NDEBUG
#define LOGD(...)
#define LOGI(...)
#else
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, HOOK_LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, HOOK_LOG_TAG, __VA_ARGS__)
#endif

#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, HOOK_LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, HOOK_LOG_TAG, __VA_ARGS__)
