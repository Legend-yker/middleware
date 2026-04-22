#pragma once

#include "dcms_export.h"
#include "api/node_handle.h"

#include <string>

namespace dcms {

// Initialize the process-level default NodeHandle and connect to DCMS Daemon.
// node_name: identifies this process in the Daemon's node table.
// Returns kDcmsOk on success.
DCMS_API int init(const std::string &node_name    = "anonymous",
                  const std::string &config_path  = "");

// Shut down and destroy the default NodeHandle.
DCMS_API int  shutdown();

// Access the process-level default NodeHandle (valid after init()).
DCMS_API NodeHandle &defaultNode();

} // namespace dcms
