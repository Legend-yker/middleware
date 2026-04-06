#pragma once

#include "dcms_export.h"

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef std::uint32_t dcms_topic_id;

typedef void (*dcms_msg_callback)(dcms_topic_id topic, const void *data,
                                  std::uint32_t len, void *user);

DCMS_API int dcms_init(const char *config_path);

DCMS_API void dcms_shutdown(void);

DCMS_API int dcms_msg_publish(dcms_topic_id topic, const void *data, std::uint32_t len);

DCMS_API int dcms_msg_subscribe(dcms_topic_id topic, dcms_msg_callback cb, void *user);
DCMS_API int dcms_msg_unsubscribe(dcms_topic_id topic, dcms_msg_callback cb, void *user);

#ifdef __cplusplus
}
#endif
