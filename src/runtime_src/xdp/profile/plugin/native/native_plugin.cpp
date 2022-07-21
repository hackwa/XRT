/**
 * Copyright (C) 2016-2022 Xilinx, Inc
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

#define XDP_SOURCE

#include "xdp/profile/plugin/native/native_plugin.h"
#include "xdp/profile/writer/native/native_writer.h"
#include "xdp/profile/plugin/vp_base/info.h"

namespace xdp {

  bool NativeProfilingPlugin::live = false;

  NativeProfilingPlugin::NativeProfilingPlugin() : XDPPlugin()
  {
    NativeProfilingPlugin::live = true ;

    db->registerPlugin(this) ;
    db->registerInfo(info::native) ;

    auto writer = std::make_unique<NativeTraceWriter>("native_trace.csv") ;
    (db->getStaticInfo()).addOpenedFile(writer->getcurrentFileName(), "VP_TRACE");
    writers.push_back(std::move(writer));
  }

  NativeProfilingPlugin::~NativeProfilingPlugin()
  {
    if (VPDatabase::alive()) {
      // Applications using the Native API may be running hardware emulation,
      //  so be sure to account for any emulation specific information
      emulationSetup() ;

      // We were destroyed before the database, so write the writers
      //  and unregister ourselves from the database
      for (const auto& w : writers)
        w->write(false) ;

      db->unregisterPlugin(this) ;
    }
    NativeProfilingPlugin::live = false;
  }

} // end namespace xdp
