#pragma once

#if defined(_WIN32) && defined(DCMS_SHARED)
#if defined(DCMS_BUILDING_LIBRARY)
#define DCMS_API __declspec(dllexport)
#else
#define DCMS_API __declspec(dllimport)
#endif
#else
#define DCMS_API
#endif
