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

#include "io/fs/remote_file_system.h"

#include "gutil/strings/stringpiece.h"
#include "io/cache/block/cached_remote_file_reader.h"
#include "io/cache/file_cache_manager.h"
#include "io/fs/file_reader_options.h"
#include "util/async_io.h"

namespace doris {
namespace io {

Status RemoteFileSystem::upload(const Path& local_file, const Path& dest_file) {
    auto dest_path = absolute_path(dest_file);
    if (bthread_self() == 0) {
        return upload_impl(local_file, dest_path);
    }
    Status s;
    auto task = [&] { s = upload_impl(local_file, dest_path); };
    AsyncIO::run_task(task, _type);
    return s;
}

Status RemoteFileSystem::batch_upload(const std::vector<Path>& local_files,
                                      const std::vector<Path>& remote_files) {
    std::vector<Path> remote_paths;
    for (auto& path : remote_files) {
        remote_paths.push_back(absolute_path(path));
    }
    if (bthread_self() == 0) {
        return batch_upload_impl(local_files, remote_paths);
    }
    Status s;
    auto task = [&] { s = batch_upload_impl(local_files, remote_paths); };
    AsyncIO::run_task(task, _type);
    return s;
}

Status RemoteFileSystem::direct_upload(const Path& remote_file, const std::string& content) {
    auto remote_path = absolute_path(remote_file);
    if (bthread_self() == 0) {
        return direct_upload_impl(remote_path, content);
    }
    Status s;
    auto task = [&] { s = direct_upload_impl(remote_path, content); };
    AsyncIO::run_task(task, _type);
    return s;
}

Status RemoteFileSystem::upload_with_checksum(const Path& local_file, const Path& remote,
                                              const std::string& checksum) {
    auto remote_path = absolute_path(remote);
    if (bthread_self() == 0) {
        return upload_with_checksum_impl(local_file, remote_path, checksum);
    }
    Status s;
    auto task = [&] { s = upload_with_checksum_impl(local_file, remote_path, checksum); };
    AsyncIO::run_task(task, _type);
    return s;
}

Status RemoteFileSystem::download(const Path& remote_file, const Path& local) {
    auto remote_path = absolute_path(remote_file);
    if (bthread_self() == 0) {
        return download_impl(remote_path, local);
    }
    Status s;
    auto task = [&] { s = download_impl(remote_path, local); };
    AsyncIO::run_task(task, _type);
    return s;
}

Status RemoteFileSystem::direct_download(const Path& remote_file, std::string* content) {
    auto remote_path = absolute_path(remote_file);
    if (bthread_self() == 0) {
        return direct_download_impl(remote_path, content);
    }
    Status s;
    auto task = [&] { s = direct_download_impl(remote_path, content); };
    AsyncIO::run_task(task, _type);
    return s;
}

Status RemoteFileSystem::connect() {
    if (bthread_self() == 0) {
        return connect_impl();
    }
    Status s;
    auto task = [&] { s = connect_impl(); };
    AsyncIO::run_task(task, _type);
    return s;
}

Status RemoteFileSystem::open_file_impl(const Path& path, const FileReaderOptions& reader_options,
                                        FileReaderSPtr* reader) {
    FileReaderSPtr raw_reader;
    RETURN_IF_ERROR(open_file_internal(path, &raw_reader));
    switch (reader_options.cache_type) {
    case io::FileCachePolicy::NO_CACHE: {
        *reader = raw_reader;
        break;
    }
    case io::FileCachePolicy::SUB_FILE_CACHE:
    case io::FileCachePolicy::WHOLE_FILE_CACHE: {
        std::string cache_path = reader_options.path_policy.get_cache_path(path.native());
        io::FileCachePtr cache_reader = FileCacheManager::instance()->new_file_cache(
                cache_path, config::file_cache_alive_time_sec, raw_reader,
                reader_options.cache_type);
        FileCacheManager::instance()->add_file_cache(cache_path, cache_reader);
        *reader = cache_reader;
        break;
    }
    case io::FileCachePolicy::FILE_BLOCK_CACHE: {
        StringPiece str(raw_reader->path().native());
        std::string cache_path = reader_options.path_policy.get_cache_path(path.native());
        *reader = std::make_shared<CachedRemoteFileReader>(std::move(raw_reader), cache_path);
        break;
    }
    default: {
        return Status::InternalError("Unknown cache type: {}", reader_options.cache_type);
    }
    }
    return Status::OK();
}

} // namespace io
} // namespace doris
