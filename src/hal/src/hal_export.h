#pragma once

#if defined(_WIN32) && defined(HAL_SHARED)
#if defined(HAL_BUILDING_LIBRARY)
#define HAL_API __declspec(dllexport)
#else
#define HAL_API __declspec(dllimport)
#endif
#else
#define HAL_API
#endif
