/**
 * Copyright (C) 2022 Advanced Micro Devices, Inc. - All rights reserved
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

#include <boost/algorithm/string.hpp>
#include <cmath>
#include <iostream>
#include <memory>

#include "core/common/message.h"
#include "core/common/xrt_profiling.h"

#include "xdp/profile/database/database.h"
#include "xdp/profile/database/events/creator/aie_trace_data_logger.h"
#include "xdp/profile/database/static_info/pl_constructs.h"
#include "xdp/profile/device/aie_trace/aie_trace_offload.h"
#include "xdp/profile/device/device_intf.h"
#include "xdp/profile/device/hal_device/xdp_hal_device.h"
#include "xdp/profile/plugin/vp_base/info.h"
#include "xdp/profile/writer/aie_trace/aie_trace_writer.h"
#include "xdp/profile/writer/aie_trace/aie_trace_config_writer.h"

// #ifdef EDGE_BUILD
// #include "edge/aie_trace.h"
// #include "core/edge/user/shim.h"
// #else
#include "x86/aie_trace.h"
// #endif

#include "aie_trace_impl.h"
#include "aie_trace_plugin.h"

namespace xdp {
  using severity_level = xrt_core::message::severity_level;
  bool AieTracePlugin::live = false;

  AieTracePlugin::AieTracePlugin() : XDPPlugin()
  {
    AieTracePlugin::live = true;

    db->registerPlugin(this);
    db->registerInfo(info::aie_trace);

    // #ifdef EDGE_BUILD
    // implementation = std::make_unique<AieTrace_EdgeImpl>(db, nullptr);
    // #else
    metadata = std::make_shared<AieTraceMetadata>();
    implementation = std::make_unique<AieTrace_x86Impl>(db, metadata);
    // #endif
  }

  AieTracePlugin::~AieTracePlugin()
  {
    if (VPDatabase::alive()) {
      try {
        writeAll(false);
      }
      catch(...) {
      }
      db->unregisterPlugin(this);
    }
    // If the database is dead, then we must have already forced a 
    //  write at the database destructor so we can just move on
    AieTracePlugin::live = false;
  }

  uint64_t AieTracePlugin::getDeviceIDFromHandle(void* handle) {
    constexpr uint32_t PATH_LENGTH = 512;

    char pathBuf[PATH_LENGTH];
    memset(pathBuf, 0, PATH_LENGTH);
    xclGetDebugIPlayoutPath(handle, pathBuf, PATH_LENGTH);
    std::string sysfspath(pathBuf);
    return db->addDevice(sysfspath); // Get the unique device Id
  }

  void AieTracePlugin::updateAIEDevice(void* handle)
  {
    if (!handle || !implementation)
      return;

    auto deviceID = getDeviceIDFromHandle(handle);
    metadata->setDeviceId(deviceID);
    metadata->HandleToDeviceID[handle] = deviceID;
    // handle is not owned by XDP

    if (!(db->getStaticInfo()).isDeviceReady(deviceID)) {
      // Delete the old offloader
      if (offloadersMap.find(deviceID) != offloadersMap.end())
        offloadersMap.erase(deviceID);

      // Update the static database with information from xclbin
      (db->getStaticInfo()).updateDevice(deviceID, handle);
      {
        struct xclDeviceInfo2 info;
        if (xclGetDeviceInfo2(handle, &info) == 0)
          (db->getStaticInfo()).setDeviceName(deviceID, std::string(info.mName));
      }
    }

    //Sets up and calls the PS kernel on x86 implementation
    //Sets up and the hardware on the edge implementation
    implementation->updateDevice(handle);

    if (!(db->getStaticInfo()).isGMIORead(deviceID)) {
      // Update the AIE specific portion of the device
      // When new xclbin is loaded, the xclbin specific datastructure is already recreated
      std::shared_ptr<xrt_core::device> device = xrt_core::get_userpf_device(handle) ;
      if (device != nullptr) {
        for (auto& gmio : metadata->get_trace_gmios(device.get()))
          (db->getStaticInfo()).addTraceGMIO(deviceID, gmio.id, gmio.shimColumn, gmio.channelNum, gmio.streamId, gmio.burstLength);
      }
      (db->getStaticInfo()).setIsGMIORead(deviceID, true);
    }

    metadata->setNumStreams((db->getStaticInfo()).getNumAIETraceStream(deviceID));
    if (metadata->getNumStreams() == 0) {
      xrt_core::message::send(severity_level::warning, "XRT", AIE_TRACE_UNAVAILABLE);
      return;
    }

    // Add all the writers
    DeviceIntf* deviceIntf = (db->getStaticInfo()).getDeviceIntf(deviceID);
    if (deviceIntf == nullptr) {
    // If DeviceIntf is not already created, create a new one to communicate with physical device
    DeviceIntf* deviceIntf = new DeviceIntf();
    try {
      deviceIntf->setDevice(new HalDevice(handle));
      deviceIntf->readDebugIPlayout();
    } catch (std::exception& e) {
      // Read debug IP layout could throw an exception
      std::stringstream msg;
      msg << "Unable to read debug IP layout for device " << deviceID << ": " << e.what();
      xrt_core::message::send(severity_level::warning, "XRT", msg.str());
      return;
    }
    (db->getStaticInfo()).setDeviceIntf(deviceID, deviceIntf);
    // configure dataflow etc. may not be required here as those are for PL side
    }

    if (implementation->isRuntimeMetrics()) {
      // This code needs to get the actual metric set so we can
      //  configure it.  I'm not sure yet what is needed for this

      std::string configFile = "aie_event_runtime_config.json";
      VPWriter* writer = new AieTraceConfigWriter
      ( configFile.c_str()
      , deviceID
      , implementation->getMetricSet()
      );
      writers.push_back(writer);
      (db->getStaticInfo()).addOpenedFile(writer->getcurrentFileName(), "AIE_EVENT_RUNTIME_CONFIG");

    }

    for (uint64_t n = 0; n < implementation->getNumStreams(); ++n) {
	    std::string fileName = "aie_trace_" + std::to_string(deviceID) + "_" + std::to_string(n) + ".txt";
      VPWriter* writer = new AIETraceWriter
      ( fileName.c_str()
      , deviceID
      , n
      , "" // version
      , "" // creation time
      , "" // xrt version
      , "" // tool version
      );
      writers.push_back(writer);
      db->getStaticInfo().addOpenedFile(writer->getcurrentFileName(), "AIE_EVENT_TRACE");

      std::stringstream msg;
      msg << "Creating AIE trace file " << fileName << " for device " << deviceID;
      xrt_core::message::send(severity_level::info, "XRT", msg.str());
    }

    // Ensure trace buffer size is appropriate
    uint64_t aieTraceBufSize = GetTS2MMBufSize(true /*isAIETrace*/);
    bool isPLIO = (db->getStaticInfo()).getNumTracePLIO(deviceID) ? true : false;

    if (implementation->getContinuousTrace()) {
      // Continuous Trace Offload is supported only for PLIO flow
      if (isPLIO)
        XDPPlugin::startWriteThread(implementation->getFileDumpIntS(), "AIE_EVENT_TRACE", false);
      else
        xrt_core::message::send(severity_level::warning, "XRT", AIE_TRACE_PERIODIC_OFFLOAD_UNSUPPORTED);
    }

    // First, check against memory bank size
    // NOTE: Check first buffer for PLIO; assume bank 0 for GMIO
    uint8_t memIndex = isPLIO ? deviceIntf->getAIETs2mmMemIndex(0) : 0;
    Memory* memory = (db->getStaticInfo()).getMemory(deviceID, memIndex);
    if (memory != nullptr) {
      uint64_t fullBankSize = memory->size * 1024;

      if ((fullBankSize > 0) && (aieTraceBufSize > fullBankSize)) {
        aieTraceBufSize = fullBankSize;
        std::string msg = "Requested AIE trace buffer is too big for memory resource. Limiting to "
                        + std::to_string(fullBankSize) + "." ;
        xrt_core::message::send(severity_level::warning, "XRT", msg);
      }
    }
    // Platform specific things like CMA
    aieTraceBufSize = implementation->checkTraceBufSize(aieTraceBufSize);

    // Create AIE Trace Offloader
    auto traceLogger = std::make_unique<AIETraceDataLogger>(deviceID);

    if (xrt_core::config::get_verbosity() >= static_cast<uint32_t>(severity_level::debug)) {
      std::string flowType = (isPLIO) ? "PLIO" : "GMIO";
      std::stringstream msg;
      msg << "Total size of " << std::fixed << std::setprecision(3)
          << (aieTraceBufSize / (1024.0 * 1024.0))
          << " MB is used for AIE trace buffer for "
          << std::to_string(implementation->getNumStreams()) << " " << flowType
          << " streams.";
      xrt_core::message::send(severity_level::debug, "XRT", msg.str());
    }

    auto offloader = std::make_unique<AIETraceOffload>
    ( handle
    , deviceID
    , deviceIntf
    , traceLogger.get()
    , isPLIO              // isPLIO?
    , aieTraceBufSize     // total trace buffer size
    , implementation->getNumStreams()
    , implementation->isEdge()
    );

    // Can't call init without setting important details in offloader
    if (implementation->getContinuousTrace() && isPLIO) {
      offloader->setContinuousTrace();
      offloader->setOffloadIntervalUs(implementation->getOffloadIntervalUs());
    }

    if (!offloader->initReadTrace()) {
      xrt_core::message::send(severity_level::warning, "XRT", AIE_TRACE_BUF_ALLOC_FAIL);
      return;
    }

    // Continuous Trace Offload is supported only for PLIO flow
    if (implementation->getContinuousTrace() && isPLIO)
      offloader->startOffload();

    offloadersMap[deviceID] = std::make_tuple
    ( std::move(offloader)
    , std::move(traceLogger)
    ,  deviceIntf
    );
  }

  void AieTracePlugin::flushAIEDevice(void* handle)
  {
    auto deviceID = getDeviceIDFromHandle(handle);
    metadata->setDeviceId(deviceID);
    metadata->HandleToDeviceID[handle] = deviceID;

    if (offloadersMap.find(deviceID) != offloadersMap.end())
      (std::get<0>(offloadersMap[deviceID]))->readTrace(true);

    if (implementation)
      implementation->flushDevice(handle);
  }

  void AieTracePlugin::finishFlushAIEDevice(void* handle)
  {
    auto deviceID = getDeviceIDFromHandle(handle);
    metadata->setDeviceId(deviceID);
    metadata->HandleToDeviceID[handle] = deviceID;

    if (offloadersMap.find(deviceID) != offloadersMap.end()) {
      auto& offloader = std::get<0>(offloadersMap[deviceID]);

      if (offloader->continuousTrace()) {
        offloader->stopOffload();
        while(offloader->getOffloadStatus() != AIEOffloadThreadStatus::STOPPED);
      }

      offloader->readTrace(true);
      if (offloader->isTraceBufferFull())
        xrt_core::message::send(severity_level::warning, "XRT", AIE_TS2MM_WARN_MSG_BUF_FULL);
      offloader->endReadTrace();
    }

    if (implementation)
      implementation->finishFlushDevice(handle);

     offloadersMap.erase(deviceID);
  }

  void AieTracePlugin::writeAll(bool openNewFiles)
  {
    for (auto& o : offloadersMap) {
      auto& offloader = std::get<0>(o.second);

      if (offloader->continuousTrace()) {
        offloader->stopOffload() ;
        while(offloader->getOffloadStatus() != AIEOffloadThreadStatus::STOPPED) ;
      }

      offloader->readTrace(true);
      if (offloader->isTraceBufferFull())
        xrt_core::message::send(severity_level::warning, "XRT", AIE_TS2MM_WARN_MSG_BUF_FULL);
      offloader->endReadTrace();
    }
    offloadersMap.clear();

    XDPPlugin::endWrite();
  }

  bool AieTracePlugin::alive()
  {
    return AieTracePlugin::live;
  }

} // end namespace xdp
