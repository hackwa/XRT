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

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream> 
#include <vector>

#include "core/common/config_reader.h"

#include "aie_trace_test.h"

aieTraceTest::
aieTraceTest()
{
  env = std::getenv("XDP_TEST_PATH");
  if (env)
    std::cout << "Testing Environment detected : " << env << std::endl;
}

bool aieTraceTest::
enabled()
{
  return (env != nullptr);
}

std::string aieTraceTest::
read_file(const std::string & path)
{
  std::ifstream is{path, std::ios::binary | std::ios::ate};
  if (!is)
    return std::string();
  std::cout << "Reading :" << path << '\n';
  auto size = is.tellg();
  std::string data(size, '\0');
  is.seekg(0);
  if (!is.read(&data[0], size))
    return std::string();
  return data;
}

void aieTraceTest::
detect_files_with_ext
(
  const std::string& dir,
  const std::string& ext,
  std::vector<std::filesystem::path>& paths
)
{
  for (auto &p : std::filesystem::recursive_directory_iterator(dir))
    if (p.path().extension() == ext)
      paths.push_back(p.path());
}

void aieTraceTest::
update_ini(const std::filesystem::path& p)
{
  std::cout << "Processing ini: " << p.string() << std::endl;
  auto ini = read_file(p.string());
  std::string key,val;
  std::stringstream ss;
  ss << ini;
  std::map <std::string, std::string> params;
  while ( ss >> key >> val ) {
    std::cout << key << " : " << val << std::endl;
    //params[key] = val; // input them into the map
    std::cout << "set " << key << " to " << val << std::endl;
    xrt_core::config::detail::set(key, val);
  }
}

void aieTraceTest::
init(AieTracePluginUnified & plugin)
{
  if (!enabled())
    return;

  auto path = std::string(env);
  auto& metadata_instance = plugin.getAIEDataForTest().begin()->second.metadata;

  std::cout << "test dir : " << path << '\n';
  std::string ini_path = path + "/xrt.ini";
  std::vector<std::filesystem::path> metadata_paths;
  std::vector<std::filesystem::path> ini_paths;

  detect_files_with_ext(path, ".json", metadata_paths);
  detect_files_with_ext(path, ".ini", ini_paths);

  for (const auto& p : ini_paths) {
    update_ini(p);
    // Test every metadata for this INI
    for (const auto& p : metadata_paths) {
      std::cout << "Processing Metadata: " << p.string() << std::endl;
      auto data = read_file(p.string());
      metadata_instance->set_test_mode();
      metadata_instance->set_test_metadata(data.c_str());
      std::cout << "[TEST] HW Gen : " << metadata_instance->getHardwareGen();
      for (auto& tileMetric : metadata_instance->getConfigMetrics()) {
        auto& metricSet = tileMetric.second;
        auto tile       = tileMetric.first;
        auto col        = tile.col;
        auto row        = tile.row;
        //auto type       = getTileType(row);
        std::cout << row << "," << col << " : " << metricSet << " ";
      }
      std::cout << std::endl;
    }
  }
}

void aieTraceTest::
finish(AieTracePluginUnified & plugin)
{
  if (!enabled())
    return;

  auto& metadata_instance = plugin.getAIEDataForTest().begin()->second.metadata;
  metadata_instance->unset_test_mode();  
}