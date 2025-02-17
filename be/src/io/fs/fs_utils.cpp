// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "io/fs/fs_utils.h"

#include <fmt/format.h>
#include <hdfs/hdfs.h>

#include <sstream>

namespace doris {
namespace io {

std::string errno_to_str() {
    char buf[1024];
    return fmt::format("({}), {}", errno, strerror_r(errno, buf, 1024));
}

std::string errcode_to_str(const std::error_code& ec) {
    return fmt::format("({}), {}", ec.value(), ec.message());
}

std::string hdfs_error() {
    std::stringstream ss;
    char buf[1024];
    ss << "(" << errno << "), " << strerror_r(errno, buf, 1024);
    ss << ", reason: " << hdfsGetLastError();
    return ss.str();
}

} // namespace io
} // namespace doris
