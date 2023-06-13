/**
 * Copyright (C) 2023 Advanced Micro Devices, Inc. - All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include <filesystem>

#include "aie_trace_plugin.h"

using namespace xdp;

class aieTraceTest {
  public:
    aieTraceTest();
    ~aieTraceTest() = default;
    void init(AieTracePluginUnified & plugin);
    void finish(AieTracePluginUnified & plugin);
  private:
    bool enabled();
    std::string read_file(const std::string & path);
    void detect_files_with_ext
    (
      const std::string& dir,
      const std::string& ext,
      std::vector<std::filesystem::path>& paths
    );
    void update_ini(const std::filesystem::path& p);
  private:
    const char* env;

};