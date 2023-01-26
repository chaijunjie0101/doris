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

#include "exprs/json_functions.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <re2/re2.h>
#include <stdlib.h>
#include <sys/time.h>

#include <boost/algorithm/string.hpp>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "common/logging.h"
#include "exprs/anyval_util.h"
#include "gutil/strings/stringpiece.h"
#include "rapidjson/error/en.h"
#include "udf/udf.h"
#include "util/string_util.h"

namespace doris {

// static const re2::RE2 JSON_PATTERN("^([a-zA-Z0-9_\\-\\:\\s#\\|\\.]*)(?:\\[([0-9]+)\\])?");
// json path cannot contains: ", [, ]
static const re2::RE2 JSON_PATTERN("^([^\\\"\\[\\]]*)(?:\\[([0-9]+|\\*)\\])?");

rapidjson::Value JsonFunctions::parse_str_with_flag(const StringVal& arg, const StringVal& flag,
                                                    const int num,
                                                    rapidjson::Document::AllocatorType& allocator) {
    rapidjson::Value val;
    if (arg.is_null || *(flag.ptr + num) == '0') { //null
        rapidjson::Value nullObject(rapidjson::kNullType);
        val = nullObject;
    } else if (*(flag.ptr + num) == '1') { //bool
        bool res = ((arg == "1") ? true : false);
        val.SetBool(res);
    } else if (*(flag.ptr + num) == '2') { //int
        std::stringstream ss;
        ss << arg.ptr;
        int number = 0;
        ss >> number;
        val.SetInt(number);
    } else if (*(flag.ptr + num) == '3') { //double
        std::stringstream ss;
        ss << arg.ptr;
        double number = 0.0;
        ss >> number;
        val.SetDouble(number);
    } else if (*(flag.ptr + num) == '4' || *(flag.ptr + num) == '5') {
        StringPiece str((char*)arg.ptr, arg.len);
        if (*(flag.ptr + num) == '4') {
            str = str.substr(1, str.length() - 2);
        }
        val.SetString(str.data(), str.length(), allocator);
    } else {
        DCHECK(false) << "parse json type error with unknown type";
    }
    return val;
}

rapidjson::Value* JsonFunctions::match_value(const std::vector<JsonPath>& parsed_paths,
                                             rapidjson::Value* document,
                                             rapidjson::Document::AllocatorType& mem_allocator,
                                             bool is_insert_null) {
    rapidjson::Value* root = document;
    rapidjson::Value* array_obj = nullptr;
    for (int i = 1; i < parsed_paths.size(); i++) {
        VLOG_TRACE << "parsed_paths: " << parsed_paths[i].debug_string();

        if (root == nullptr || root->IsNull()) {
            return nullptr;
        }

        if (UNLIKELY(!parsed_paths[i].is_valid)) {
            return nullptr;
        }

        const std::string& col = parsed_paths[i].key;
        int index = parsed_paths[i].idx;
        if (LIKELY(!col.empty())) {
            if (root->IsArray()) {
                array_obj = static_cast<rapidjson::Value*>(
                        mem_allocator.Malloc(sizeof(rapidjson::Value)));
                array_obj->SetArray();
                bool is_null = true;

                // if array ,loop the array,find out all Objects,then find the results from the objects
                for (int j = 0; j < root->Size(); j++) {
                    rapidjson::Value* json_elem = &((*root)[j]);

                    if (json_elem->IsArray() || json_elem->IsNull()) {
                        continue;
                    } else {
                        if (!json_elem->IsObject()) {
                            continue;
                        }
                        if (!json_elem->HasMember(col.c_str())) {
                            if (is_insert_null) { // not found item, then insert a null object.
                                is_null = false;
                                rapidjson::Value nullObject(rapidjson::kNullType);
                                array_obj->PushBack(nullObject, mem_allocator);
                            }
                            continue;
                        }
                        rapidjson::Value* obj = &((*json_elem)[col.c_str()]);
                        if (obj->IsArray()) {
                            is_null = false;
                            for (int k = 0; k < obj->Size(); k++) {
                                array_obj->PushBack((*obj)[k], mem_allocator);
                            }
                        } else if (!obj->IsNull()) {
                            is_null = false;
                            array_obj->PushBack(*obj, mem_allocator);
                        }
                    }
                }

                root = is_null ? &(array_obj->SetNull()) : array_obj;
            } else if (root->IsObject()) {
                if (!root->HasMember(col.c_str())) {
                    return nullptr;
                } else {
                    root = &((*root)[col.c_str()]);
                }
            } else {
                // root is not a nested type, return nullptr
                return nullptr;
            }
        }

        if (UNLIKELY(index != -1)) {
            // judge the rapidjson:Value, which base the top's result,
            // if not array return nullptr;else get the index value from the array
            if (root->IsArray()) {
                if (root->IsNull()) {
                    return nullptr;
                } else if (index == -2) {
                    // [*]
                    array_obj = static_cast<rapidjson::Value*>(
                            mem_allocator.Malloc(sizeof(rapidjson::Value)));
                    array_obj->SetArray();

                    for (int j = 0; j < root->Size(); j++) {
                        rapidjson::Value v;
                        v.CopyFrom((*root)[j], mem_allocator);
                        array_obj->PushBack(v, mem_allocator);
                    }
                    root = array_obj;
                } else if (index >= root->Size()) {
                    return nullptr;
                } else {
                    root = &((*root)[index]);
                }
            } else {
                return nullptr;
            }
        }
    }
    return root;
}

rapidjson::Value* JsonFunctions::get_json_array_from_parsed_json(
        const std::string& json_path, rapidjson::Value* document,
        rapidjson::Document::AllocatorType& mem_allocator, bool* wrap_explicitly) {
    std::vector<JsonPath> vec;
    parse_json_paths(json_path, &vec);
    return get_json_array_from_parsed_json(vec, document, mem_allocator, wrap_explicitly);
}

rapidjson::Value* JsonFunctions::get_json_array_from_parsed_json(
        const std::vector<JsonPath>& parsed_paths, rapidjson::Value* document,
        rapidjson::Document::AllocatorType& mem_allocator, bool* wrap_explicitly) {
    *wrap_explicitly = false;
    if (!parsed_paths[0].is_valid) {
        return nullptr;
    }

    if (parsed_paths.size() == 1) {
        // the json path is "$", just return entire document
        // wrapper an array
        rapidjson::Value* array_obj = nullptr;
        array_obj = static_cast<rapidjson::Value*>(mem_allocator.Malloc(sizeof(rapidjson::Value)));
        array_obj->SetArray();
        array_obj->PushBack(*document, mem_allocator);
        return array_obj;
    }

    rapidjson::Value* root = match_value(parsed_paths, document, mem_allocator, true);
    if (root == nullptr || root == document) { // not found
        return nullptr;
    } else if (!root->IsArray()) {
        rapidjson::Value* array_obj = nullptr;
        array_obj = static_cast<rapidjson::Value*>(mem_allocator.Malloc(sizeof(rapidjson::Value)));
        array_obj->SetArray();
        array_obj->PushBack(*root, mem_allocator);
        // set `wrap_explicitly` to true, so that the caller knows that this Array is wrapped actively.
        *wrap_explicitly = true;
        return array_obj;
    }
    return root;
}

rapidjson::Value* JsonFunctions::get_json_object_from_parsed_json(
        const std::vector<JsonPath>& parsed_paths, rapidjson::Value* document,
        rapidjson::Document::AllocatorType& mem_allocator) {
    if (!parsed_paths[0].is_valid) {
        return nullptr;
    }

    if (parsed_paths.size() == 1) {
        // the json path is "$", just return entire document
        return document;
    }

    rapidjson::Value* root = match_value(parsed_paths, document, mem_allocator, true);
    if (root == nullptr || root == document) { // not found
        return nullptr;
    }
    return root;
}

void JsonFunctions::parse_json_paths(const std::string& path_string,
                                     std::vector<JsonPath>* parsed_paths) {
    // split path by ".", and escape quota by "\"
    // eg:
    //    '$.text#abc.xyz'  ->  [$, text#abc, xyz]
    //    '$."text.abc".xyz'  ->  [$, text.abc, xyz]
    //    '$."text.abc"[1].xyz'  ->  [$, text.abc[1], xyz]
    boost::tokenizer<boost::escaped_list_separator<char>> tok(
            path_string, boost::escaped_list_separator<char>("\\", ".", "\""));
    std::vector<std::string> paths(tok.begin(), tok.end());
    get_parsed_paths(paths, parsed_paths);
}

void JsonFunctions::get_parsed_paths(const std::vector<std::string>& path_exprs,
                                     std::vector<JsonPath>* parsed_paths) {
    if (path_exprs.empty()) {
        return;
    }

    if (path_exprs[0] != "$") {
        parsed_paths->emplace_back("", -1, false);
    } else {
        parsed_paths->emplace_back("$", -1, true);
    }

    for (int i = 1; i < path_exprs.size(); i++) {
        std::string col;
        std::string index;
        if (UNLIKELY(!RE2::FullMatch(path_exprs[i], JSON_PATTERN, &col, &index))) {
            parsed_paths->emplace_back("", -1, false);
        } else {
            int idx = -1;
            if (!index.empty()) {
                if (index == "*") {
                    idx = -2;
                } else {
                    idx = atoi(index.c_str());
                }
            }
            parsed_paths->emplace_back(std::move(col), idx, true);
        }
    }
}

} // namespace doris
