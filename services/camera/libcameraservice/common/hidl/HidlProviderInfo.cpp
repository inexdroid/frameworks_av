/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "HidlProviderInfo.h"

#include <cutils/properties.h>

#include <android/hardware/ICameraService.h>
#include <camera_metadata_hidden.h>

#include "device3/ZoomRatioMapper.h"
#include <utils/SessionConfigurationUtils.h>
#include <utils/Trace.h>

#include <android/hardware/camera/device/3.7/ICameraDevice.h>
#include <android/hardware/camera/device/3.8/ICameraDevice.h>

namespace {
const bool kEnableLazyHal(property_get_bool("ro.camera.enableLazyHal", false));
} // anonymous namespace

namespace android {

using namespace android::camera3;
using namespace hardware::camera;
using hardware::camera::common::V1_0::VendorTagSection;
using hardware::camera::common::V1_0::Status;
using hardware::camera::provider::V2_7::CameraIdAndStreamCombination;
using hardware::camera2::utils::CameraIdAndSessionConfiguration;


using StatusListener = CameraProviderManager::StatusListener;

using hardware::camera::provider::V2_5::DeviceState;
using hardware::ICameraService;

status_t HidlProviderInfo::mapToStatusT(const Status& s)  {
    switch(s) {
        case Status::OK:
            return OK;
        case Status::ILLEGAL_ARGUMENT:
            return BAD_VALUE;
        case Status::CAMERA_IN_USE:
            return -EBUSY;
        case Status::MAX_CAMERAS_IN_USE:
            return -EUSERS;
        case Status::METHOD_NOT_SUPPORTED:
            return UNKNOWN_TRANSACTION;
        case Status::OPERATION_NOT_SUPPORTED:
            return INVALID_OPERATION;
        case Status::CAMERA_DISCONNECTED:
            return DEAD_OBJECT;
        case Status::INTERNAL_ERROR:
            return INVALID_OPERATION;
    }
    ALOGW("Unexpected HAL status code %d", s);
    return INVALID_OPERATION;
}

static hardware::hidl_bitfield<DeviceState> mapToHidlDeviceState(int64_t newState) {
    hardware::hidl_bitfield<DeviceState> newDeviceState{};
    if (newState & ICameraService::DEVICE_STATE_BACK_COVERED) {
        newDeviceState |= DeviceState::BACK_COVERED;
    }
    if (newState & ICameraService::DEVICE_STATE_FRONT_COVERED) {
        newDeviceState |= DeviceState::FRONT_COVERED;
    }
    if (newState & ICameraService::DEVICE_STATE_FOLDED) {
        newDeviceState |= DeviceState::FOLDED;
    }
    // Only map vendor bits directly
    uint64_t vendorBits = static_cast<uint64_t>(newState) & 0xFFFFFFFF00000000l;
    newDeviceState |= vendorBits;

    ALOGV("%s: New device state 0x%" PRIx64, __FUNCTION__, newDeviceState);
    return newDeviceState;
}

const char* statusToString(const Status& s) {
    switch(s) {
        case Status::OK:
            return "OK";
        case Status::ILLEGAL_ARGUMENT:
            return "ILLEGAL_ARGUMENT";
        case Status::CAMERA_IN_USE:
            return "CAMERA_IN_USE";
        case Status::MAX_CAMERAS_IN_USE:
            return "MAX_CAMERAS_IN_USE";
        case Status::METHOD_NOT_SUPPORTED:
            return "METHOD_NOT_SUPPORTED";
        case Status::OPERATION_NOT_SUPPORTED:
            return "OPERATION_NOT_SUPPORTED";
        case Status::CAMERA_DISCONNECTED:
            return "CAMERA_DISCONNECTED";
        case Status::INTERNAL_ERROR:
            return "INTERNAL_ERROR";
    }
    ALOGW("Unexpected HAL status code %d", s);
    return "UNKNOWN_ERROR";
}

static common::V1_0::CameraDeviceStatus mapToHidlCameraDeviceStatus(const CameraDeviceStatus& s)  {
    switch(s) {
        case CameraDeviceStatus::PRESENT:
            return common::V1_0::CameraDeviceStatus::PRESENT;
        case CameraDeviceStatus::NOT_PRESENT:
            return common::V1_0::CameraDeviceStatus::NOT_PRESENT;
        case CameraDeviceStatus::ENUMERATING:
            return common::V1_0::CameraDeviceStatus::ENUMERATING;
    }
    ALOGW("Unexpectedcamera device status code %d", s);
    return common::V1_0::CameraDeviceStatus::NOT_PRESENT;
}

static CameraDeviceStatus hidlToInternalCameraDeviceStatus(
        const common::V1_0::CameraDeviceStatus& s)  {
    switch(s) {
        case common::V1_0::CameraDeviceStatus::PRESENT:
            return CameraDeviceStatus::PRESENT;
        case common::V1_0::CameraDeviceStatus::NOT_PRESENT:
            return CameraDeviceStatus::NOT_PRESENT;
        case common::V1_0::CameraDeviceStatus::ENUMERATING:
            return CameraDeviceStatus::ENUMERATING;
    }
    ALOGW("Unexpectedcamera device status code %d", s);
    return CameraDeviceStatus::NOT_PRESENT;
}

static TorchModeStatus hidlToInternalTorchModeStatus(
        const common::V1_0::TorchModeStatus& s)  {
    switch(s) {
        case common::V1_0::TorchModeStatus::NOT_AVAILABLE:
            return TorchModeStatus::NOT_AVAILABLE;
        case common::V1_0::TorchModeStatus::AVAILABLE_OFF:
            return TorchModeStatus::AVAILABLE_OFF;
        case common::V1_0::TorchModeStatus::AVAILABLE_ON:
            return TorchModeStatus::AVAILABLE_ON;
    }
    ALOGW("Unexpectedcamera torch mode status code %d", s);
    return TorchModeStatus::NOT_AVAILABLE;
}

static CameraResourceCost hidlToInternalResourceCost(
        const common::V1_0::CameraResourceCost& s)  {
    CameraResourceCost internalResourceCost;
    internalResourceCost.resourceCost = s.resourceCost;
    for (const auto device : s.conflictingDevices) {
        internalResourceCost.conflictingDevices.emplace_back(device.c_str());
    }
    return internalResourceCost;
}

static const char* deviceStatusToString(const common::V1_0::CameraDeviceStatus& s) {
    switch(s) {
        case common::V1_0::CameraDeviceStatus::NOT_PRESENT:
            return "NOT_PRESENT";
        case common::V1_0::CameraDeviceStatus::PRESENT:
            return "PRESENT";
        case common::V1_0::CameraDeviceStatus::ENUMERATING:
            return "ENUMERATING";
    }
    ALOGW("Unexpected HAL device status code %d", s);
    return "UNKNOWN_STATUS";
}

static const char* torchStatusToString(const common::V1_0::TorchModeStatus& s) {
    switch(s) {
        case common::V1_0::TorchModeStatus::NOT_AVAILABLE:
            return "NOT_AVAILABLE";
        case common::V1_0::TorchModeStatus::AVAILABLE_OFF:
            return "AVAILABLE_OFF";
        case common::V1_0::TorchModeStatus::AVAILABLE_ON:
            return "AVAILABLE_ON";
    }
    ALOGW("Unexpected HAL torch mode status code %d", s);
    return "UNKNOWN_STATUS";
}

status_t HidlProviderInfo::initializeHidlProvider(
        sp<provider::V2_4::ICameraProvider>& interface,
        int64_t currentDeviceState) {
    status_t res = parseProviderName(mProviderName, &mType, &mId);
    if (res != OK) {
        ALOGE("%s: Invalid provider name, ignoring", __FUNCTION__);
        return BAD_VALUE;
    }
    ALOGI("Connecting to new camera provider: %s, isRemote? %d",
            mProviderName.c_str(), interface->isRemote());

    // Determine minor version
    mMinorVersion = 4;
    auto cast2_6 = provider::V2_6::ICameraProvider::castFrom(interface);
    sp<provider::V2_6::ICameraProvider> interface2_6 = nullptr;
    if (cast2_6.isOk()) {
        interface2_6 = cast2_6;
        if (interface2_6 != nullptr) {
            mMinorVersion = 6;
        }
    }
    // We need to check again since cast2_6.isOk() succeeds even if the provider
    // version isn't actually 2.6.
    if (interface2_6 == nullptr){
        auto cast2_5 =
                provider::V2_5::ICameraProvider::castFrom(interface);
        sp<provider::V2_5::ICameraProvider> interface2_5 = nullptr;
        if (cast2_5.isOk()) {
            interface2_5 = cast2_5;
            if (interface != nullptr) {
                mMinorVersion = 5;
            }
        }
    } else {
        auto cast2_7 = provider::V2_7::ICameraProvider::castFrom(interface);
        if (cast2_7.isOk()) {
            sp<provider::V2_7::ICameraProvider> interface2_7 = cast2_7;
            if (interface2_7 != nullptr) {
                mMinorVersion = 7;
            }
        }
    }

    // cameraDeviceStatusChange callbacks may be called (and causing new devices added)
    // before setCallback returns
    hardware::Return<Status> status = interface->setCallback(this);
    if (!status.isOk()) {
        ALOGE("%s: Transaction error setting up callbacks with camera provider '%s': %s",
                __FUNCTION__, mProviderName.c_str(), status.description().c_str());
        return DEAD_OBJECT;
    }
    if (status != Status::OK) {
        ALOGE("%s: Unable to register callbacks with camera provider '%s'",
                __FUNCTION__, mProviderName.c_str());
        return mapToStatusT(status);
    }

    hardware::Return<bool> linked = interface->linkToDeath(this, /*cookie*/ mId);
    if (!linked.isOk()) {
        ALOGE("%s: Transaction error in linking to camera provider '%s' death: %s",
                __FUNCTION__, mProviderName.c_str(), linked.description().c_str());
        return DEAD_OBJECT;
    } else if (!linked) {
        ALOGW("%s: Unable to link to provider '%s' death notifications",
                __FUNCTION__, mProviderName.c_str());
    }

    if (!kEnableLazyHal) {
        // Save HAL reference indefinitely
        mSavedInterface = interface;
    } else {
        mActiveInterface = interface;
    }

    ALOGV("%s: Setting device state for %s: 0x%" PRIx64,
            __FUNCTION__, mProviderName.c_str(), mDeviceState);
    notifyDeviceStateChange(currentDeviceState);

    res = setUpVendorTags();
    if (res != OK) {
        ALOGE("%s: Unable to set up vendor tags from provider '%s'",
                __FUNCTION__, mProviderName.c_str());
        return res;
    }

    // Get initial list of camera devices, if any
    std::vector<std::string> devices;
    hardware::Return<void> ret = interface->getCameraIdList([&status, this, &devices](
            Status idStatus,
            const hardware::hidl_vec<hardware::hidl_string>& cameraDeviceNames) {
        status = idStatus;
        if (status == Status::OK) {
            for (auto& name : cameraDeviceNames) {
                uint16_t major, minor;
                std::string type, id;
                status_t res = parseDeviceName(name, &major, &minor, &type, &id);
                if (res != OK) {
                    ALOGE("%s: Error parsing deviceName: %s: %d", __FUNCTION__, name.c_str(), res);
                    status = Status::INTERNAL_ERROR;
                } else {
                    devices.push_back(name);
                    mProviderPublicCameraIds.push_back(id);
                }
            }
        } });
    if (!ret.isOk()) {
        ALOGE("%s: Transaction error in getting camera ID list from provider '%s': %s",
                __FUNCTION__, mProviderName.c_str(), linked.description().c_str());
        return DEAD_OBJECT;
    }
    if (status != Status::OK) {
        ALOGE("%s: Unable to query for camera devices from provider '%s'",
                __FUNCTION__, mProviderName.c_str());
        return mapToStatusT(status);
    }

    // Get list of concurrent streaming camera device combinations
    if (mMinorVersion >= 6) {
        res = getConcurrentCameraIdsInternalLocked(interface2_6);
        if (res != OK) {
            return res;
        }
    }

    ret = interface->isSetTorchModeSupported(
        [this](auto status, bool supported) {
            if (status == Status::OK) {
                mSetTorchModeSupported = supported;
            }
        });
    if (!ret.isOk()) {
        ALOGE("%s: Transaction error checking torch mode support '%s': %s",
                __FUNCTION__, mProviderName.c_str(), ret.description().c_str());
        return DEAD_OBJECT;
    }

    mIsRemote = interface->isRemote();

    sp<StatusListener> listener = mManager->getStatusListener();
    for (auto& device : devices) {
        std::string id;
        status_t res = addDevice(device, common::V1_0::CameraDeviceStatus::PRESENT, &id);
        if (res != OK) {
            ALOGE("%s: Unable to enumerate camera device '%s': %s (%d)",
                    __FUNCTION__, device.c_str(), strerror(-res), res);
            continue;
        }
    }

    ALOGI("Camera provider %s ready with %zu camera devices",
            mProviderName.c_str(), mDevices.size());

    // Process cached status callbacks
    std::unique_ptr<std::vector<CameraStatusInfoT>> cachedStatus =
            std::make_unique<std::vector<CameraStatusInfoT>>();
    {
        std::lock_guard<std::mutex> lock(mInitLock);

        for (auto& statusInfo : mCachedStatus) {
            std::string id, physicalId;
            status_t res = OK;
            if (statusInfo.isPhysicalCameraStatus) {
                res = physicalCameraDeviceStatusChangeLocked(&id, &physicalId,
                    statusInfo.cameraId, statusInfo.physicalCameraId,
                    mapToHidlCameraDeviceStatus(statusInfo.status));
            } else {
                res = cameraDeviceStatusChangeLocked(&id, statusInfo.cameraId,
                        mapToHidlCameraDeviceStatus(statusInfo.status));
            }
            if (res == OK) {
                cachedStatus->emplace_back(statusInfo.isPhysicalCameraStatus,
                        id.c_str(), physicalId.c_str(), statusInfo.status);
            }
        }
        mCachedStatus.clear();

        mInitialized = true;
    }

    // The cached status change callbacks cannot be fired directly from this
    // function, due to same-thread deadlock trying to acquire mInterfaceMutex
    // twice.
    if (listener != nullptr) {
        mInitialStatusCallbackFuture = std::async(std::launch::async,
                &CameraProviderManager::ProviderInfo::notifyInitialStatusChange, this,
                listener, std::move(cachedStatus));
    }

    return OK;
}

status_t HidlProviderInfo::setUpVendorTags() {
    if (mVendorTagDescriptor != nullptr)
        return OK;

    hardware::hidl_vec<VendorTagSection> vts;
    Status status;
    hardware::Return<void> ret;
    const sp<hardware::camera::provider::V2_4::ICameraProvider> interface =
            startProviderInterface();
    if (interface == nullptr) {
        return DEAD_OBJECT;
    }
    ret = interface->getVendorTags(
        [&](auto s, const auto& vendorTagSecs) {
            status = s;
            if (s == Status::OK) {
                vts = vendorTagSecs;
            }
    });
    if (!ret.isOk()) {
        ALOGE("%s: Transaction error getting vendor tags from provider '%s': %s",
                __FUNCTION__, mProviderName.c_str(), ret.description().c_str());
        return DEAD_OBJECT;
    }
    if (status != Status::OK) {
        return mapToStatusT(status);
    }

    // Read all vendor tag definitions into a descriptor
    status_t res;
    if ((res = HidlVendorTagDescriptor::createDescriptorFromHidl(vts, /*out*/mVendorTagDescriptor))
            != OK) {
        ALOGE("%s: Could not generate descriptor from vendor tag operations,"
                "received error %s (%d). Camera clients will not be able to use"
                "vendor tags", __FUNCTION__, strerror(res), res);
        return res;
    }

    return OK;
}

status_t HidlProviderInfo::notifyDeviceStateChange(int64_t newDeviceState) {
    mDeviceState = mapToHidlDeviceState(newDeviceState);
    if (mMinorVersion >= 5) {
        // Check if the provider is currently active - not going to start it for this notification
        auto interface = mSavedInterface != nullptr ? mSavedInterface : mActiveInterface.promote();
        if (interface != nullptr) {
            // Send current device state
            auto castResult = provider::V2_5::ICameraProvider::castFrom(interface);
            if (castResult.isOk()) {
                sp<provider::V2_5::ICameraProvider> interface_2_5 = castResult;
                if (interface_2_5 != nullptr) {
                    interface_2_5->notifyDeviceStateChange(mDeviceState);
                }
            }
        }
    }
    return OK;
}

sp<device::V3_2::ICameraDevice>
HidlProviderInfo::startDeviceInterface(const std::string &name) {
    Status status;
    sp<device::V3_2::ICameraDevice> cameraInterface;
    hardware::Return<void> ret;
    const sp<provider::V2_4::ICameraProvider> interface = startProviderInterface();
    if (interface == nullptr) {
        return nullptr;
    }
    ret = interface->getCameraDeviceInterface_V3_x(name, [&status, &cameraInterface](
        Status s, sp<device::V3_2::ICameraDevice> interface) {
                status = s;
                cameraInterface = interface;
            });
    if (!ret.isOk()) {
        ALOGE("%s: Transaction error trying to obtain interface for camera device %s: %s",
                __FUNCTION__, name.c_str(), ret.description().c_str());
        return nullptr;
    }
    if (status != Status::OK) {
        ALOGE("%s: Unable to obtain interface for camera device %s: %s", __FUNCTION__,
                name.c_str(), statusToString(status));
        return nullptr;
    }
    return cameraInterface;
}

bool HidlProviderInfo::successfullyStartedProviderInterface() {
    return startProviderInterface() != nullptr;
}

const sp<provider::V2_4::ICameraProvider>
HidlProviderInfo::startProviderInterface() {
    ATRACE_CALL();
    ALOGV("Request to start camera provider: %s", mProviderName.c_str());
    if (mSavedInterface != nullptr) {
        return mSavedInterface;
    }
    if (!kEnableLazyHal) {
        ALOGE("Bad provider state! Should not be here on a non-lazy HAL!");
        return nullptr;
    }

    auto interface = mActiveInterface.promote();
    if (interface == nullptr) {
        // Try to get service without starting
        interface = mManager->mHidlServiceProxy->tryGetService(mProviderName);
        if (interface == nullptr) {
            ALOGV("Camera provider actually needs restart, calling getService(%s)",
                  mProviderName.c_str());
            interface = mManager->mHidlServiceProxy->getService(mProviderName);

            // Set all devices as ENUMERATING, provider should update status
            // to PRESENT after initializing.
            // This avoids failing getCameraDeviceInterface_V3_x before devices
            // are ready.
            for (auto& device : mDevices) {
              device->mIsDeviceAvailable = false;
            }

            interface->setCallback(this);
            hardware::Return<bool>
                linked = interface->linkToDeath(this, /*cookie*/ mId);
            if (!linked.isOk()) {
              ALOGE(
                  "%s: Transaction error in linking to camera provider '%s' death: %s",
                  __FUNCTION__,
                  mProviderName.c_str(),
                  linked.description().c_str());
              mManager->removeProvider(mProviderName);
              return nullptr;
            } else if (!linked) {
              ALOGW("%s: Unable to link to provider '%s' death notifications",
                    __FUNCTION__, mProviderName.c_str());
            }
            // Send current device state
            if (mMinorVersion >= 5) {
              auto castResult =
                  provider::V2_5::ICameraProvider::castFrom(interface);
              if (castResult.isOk()) {
                sp<provider::V2_5::ICameraProvider> interface_2_5 = castResult;
                if (interface_2_5 != nullptr) {
                  ALOGV("%s: Initial device state for %s: 0x %" PRIx64,
                        __FUNCTION__, mProviderName.c_str(), mDeviceState);
                  interface_2_5->notifyDeviceStateChange(mDeviceState);
                }
              }
            }
        }
        mActiveInterface = interface;
    } else {
        ALOGV("Camera provider (%s) already in use. Re-using instance.",
              mProviderName.c_str());
    }

    return interface;
}

hardware::Return<void> HidlProviderInfo::cameraDeviceStatusChange(
        const hardware::hidl_string& cameraDeviceName,
        hardware::camera::common::V1_0::CameraDeviceStatus newStatus) {
    sp<StatusListener> listener;
    std::string id;
    std::lock_guard<std::mutex> lock(mInitLock);
    CameraDeviceStatus internalNewStatus = hidlToInternalCameraDeviceStatus(newStatus);
    if (!mInitialized) {
        mCachedStatus.emplace_back(false /*isPhysicalCameraStatus*/,
                cameraDeviceName.c_str(), std::string().c_str(),
                internalNewStatus);
        return hardware::Void();
    }

    {
        std::lock_guard<std::mutex> lock(mLock);
        if (OK != cameraDeviceStatusChangeLocked(&id, cameraDeviceName, newStatus)) {
            return hardware::Void();
        }
        listener = mManager->getStatusListener();
    }

    // Call without lock held to allow reentrancy into provider manager
    if (listener != nullptr) {
        listener->onDeviceStatusChanged(String8(id.c_str()), internalNewStatus);
    }

    return hardware::Void();
}

status_t HidlProviderInfo::addDevice(const std::string& name,
        common::V1_0::CameraDeviceStatus initialStatus, /*out*/ std::string* parsedId) {

    ALOGI("Enumerating new camera device: %s", name.c_str());

    uint16_t major, minor;
    std::string type, id;

    status_t res = parseDeviceName(name, &major, &minor, &type, &id);
    if (res != OK) {
        return res;
    }
    if (type != mType) {
        ALOGE("%s: Device type %s does not match provider type %s", __FUNCTION__,
                type.c_str(), mType.c_str());
        return BAD_VALUE;
    }
    if (mManager->isValidDeviceLocked(id, major)) {
        ALOGE("%s: Device %s: ID %s is already in use for device major version %d", __FUNCTION__,
                name.c_str(), id.c_str(), major);
        return BAD_VALUE;
    }

    std::unique_ptr<DeviceInfo> deviceInfo;
    switch (major) {
        case 1:
            ALOGE("%s: Device %s: Unsupported HIDL device HAL major version %d:", __FUNCTION__,
                    name.c_str(), major);
            return BAD_VALUE;
        case 3:
            deviceInfo = initializeDeviceInfo(name, mProviderTagid, id, minor);
            break;
        default:
            ALOGE("%s: Device %s: Unknown HIDL device HAL major version %d:", __FUNCTION__,
                    name.c_str(), major);
            return BAD_VALUE;
    }
    if (deviceInfo == nullptr) return BAD_VALUE;
    deviceInfo->notifyDeviceStateChange(mDeviceState);
    deviceInfo->mStatus = hidlToInternalCameraDeviceStatus(initialStatus);
    bool isAPI1Compatible = deviceInfo->isAPI1Compatible();

    mDevices.push_back(std::move(deviceInfo));

    mUniqueCameraIds.insert(id);
    if (isAPI1Compatible) {
        // addDevice can be called more than once for the same camera id if HAL
        // supports openLegacy.
        if (std::find(mUniqueAPI1CompatibleCameraIds.begin(), mUniqueAPI1CompatibleCameraIds.end(),
                id) == mUniqueAPI1CompatibleCameraIds.end()) {
            mUniqueAPI1CompatibleCameraIds.push_back(id);
        }
    }

    if (parsedId != nullptr) {
        *parsedId = id;
    }
    return OK;
}

status_t HidlProviderInfo::cameraDeviceStatusChangeLocked(
        std::string* id, const hardware::hidl_string& cameraDeviceName,
        hardware::camera::common::V1_0::CameraDeviceStatus newStatus) {
    bool known = false;
    std::string cameraId;
    for (auto& deviceInfo : mDevices) {
        if (deviceInfo->mName == cameraDeviceName) {
            Mutex::Autolock l(deviceInfo->mDeviceAvailableLock);
            ALOGI("Camera device %s status is now %s, was %s", cameraDeviceName.c_str(),
                    deviceStatusToString(newStatus),
                    deviceStatusToString(mapToHidlCameraDeviceStatus(deviceInfo->mStatus)));
            deviceInfo->mStatus = hidlToInternalCameraDeviceStatus(newStatus);
            // TODO: Handle device removal (NOT_PRESENT)
            cameraId = deviceInfo->mId;
            known = true;
            deviceInfo->mIsDeviceAvailable =
                (newStatus == hardware::camera::common::V1_0::CameraDeviceStatus::PRESENT);
            deviceInfo->mDeviceAvailableSignal.signal();
            break;
        }
    }
    // Previously unseen device; status must not be NOT_PRESENT
    if (!known) {
        if (newStatus == hardware::camera::common::V1_0::CameraDeviceStatus::NOT_PRESENT) {
            ALOGW("Camera provider %s says an unknown camera device %s is not present. Curious.",
                mProviderName.c_str(), cameraDeviceName.c_str());
            return BAD_VALUE;
        }
        addDevice(cameraDeviceName, newStatus, &cameraId);
    } else if (newStatus == hardware::camera::common::V1_0::CameraDeviceStatus::NOT_PRESENT) {
        removeDevice(cameraId);
    } else if (isExternalLazyHAL()) {
        // Do not notify CameraService for PRESENT->PRESENT (lazy HAL restart)
        // because NOT_AVAILABLE is set on CameraService::connect and a PRESENT
        // notif. would overwrite it
        return BAD_VALUE;
    }
    if (reCacheConcurrentStreamingCameraIdsLocked() != OK) {
        ALOGE("%s: CameraProvider %s could not re-cache concurrent streaming camera id list ",
                  __FUNCTION__, mProviderName.c_str());
    }
    *id = cameraId;
    return OK;
}

hardware::Return<void> HidlProviderInfo::physicalCameraDeviceStatusChange(
        const hardware::hidl_string& cameraDeviceName,
        const hardware::hidl_string& physicalCameraDeviceName,
        hardware::camera::common::V1_0::CameraDeviceStatus newStatus) {
    sp<StatusListener> listener;
    std::string id;
    std::string physicalId;
    std::lock_guard<std::mutex> lock(mInitLock);
    CameraDeviceStatus newInternalStatus = hidlToInternalCameraDeviceStatus(newStatus);
    if (!mInitialized) {
        mCachedStatus.emplace_back(true /*isPhysicalCameraStatus*/, cameraDeviceName,
                physicalCameraDeviceName, newInternalStatus);
        return hardware::Void();
    }

    {
        std::lock_guard<std::mutex> lock(mLock);

        if (OK != physicalCameraDeviceStatusChangeLocked(&id, &physicalId, cameraDeviceName,
                physicalCameraDeviceName, newStatus)) {
            return hardware::Void();
        }

        listener = mManager->getStatusListener();
    }
    // Call without lock held to allow reentrancy into provider manager
    if (listener != nullptr) {
        listener->onDeviceStatusChanged(String8(id.c_str()),
                String8(physicalId.c_str()), newInternalStatus);
    }
    return hardware::Void();
}

status_t HidlProviderInfo::physicalCameraDeviceStatusChangeLocked(
            std::string* id, std::string* physicalId,
            const hardware::hidl_string& cameraDeviceName,
            const hardware::hidl_string& physicalCameraDeviceName,
            hardware::camera::common::V1_0::CameraDeviceStatus newStatus) {
    bool known = false;
    std::string cameraId;
    for (auto& deviceInfo : mDevices) {
        if (deviceInfo->mName == cameraDeviceName) {
            cameraId = deviceInfo->mId;
            if (!deviceInfo->mIsLogicalCamera) {
                ALOGE("%s: Invalid combination of camera id %s, physical id %s",
                        __FUNCTION__, cameraId.c_str(), physicalCameraDeviceName.c_str());
                return BAD_VALUE;
            }
            if (std::find(deviceInfo->mPhysicalIds.begin(), deviceInfo->mPhysicalIds.end(),
                    physicalCameraDeviceName) == deviceInfo->mPhysicalIds.end()) {
                ALOGE("%s: Invalid combination of camera id %s, physical id %s",
                        __FUNCTION__, cameraId.c_str(), physicalCameraDeviceName.c_str());
                return BAD_VALUE;
            }
            ALOGI("Camera device %s physical device %s status is now %s",
                    cameraDeviceName.c_str(), physicalCameraDeviceName.c_str(),
                    deviceStatusToString(newStatus));
            known = true;
            break;
        }
    }
    // Previously unseen device; status must not be NOT_PRESENT
    if (!known) {
        ALOGW("Camera provider %s says an unknown camera device %s-%s is not present. Curious.",
                mProviderName.c_str(), cameraDeviceName.c_str(),
                physicalCameraDeviceName.c_str());
        return BAD_VALUE;
    }

    *id = cameraId;
    *physicalId = physicalCameraDeviceName.c_str();
    return OK;
}

hardware::Return<void> HidlProviderInfo::torchModeStatusChange(
        const hardware::hidl_string& cameraDeviceName,
        hardware::camera::common::V1_0::TorchModeStatus newStatus) {
    sp<StatusListener> listener;
    SystemCameraKind systemCameraKind = SystemCameraKind::PUBLIC;
    std::string id;
    bool known = false;
    {
        // Hold mLock for accessing mDevices
        std::lock_guard<std::mutex> lock(mLock);
        for (auto& deviceInfo : mDevices) {
            if (deviceInfo->mName == cameraDeviceName) {
                ALOGI("Camera device %s torch status is now %s", cameraDeviceName.c_str(),
                        torchStatusToString(newStatus));
                id = deviceInfo->mId;
                known = true;
                systemCameraKind = deviceInfo->mSystemCameraKind;
                if (hardware::camera::common::V1_0::TorchModeStatus::AVAILABLE_ON != newStatus) {
                    mManager->removeRef(CameraProviderManager::DeviceMode::TORCH, id);
                }
                break;
            }
        }
        if (!known) {
            ALOGW("Camera provider %s says an unknown camera %s now has torch status %d. Curious.",
                    mProviderName.c_str(), cameraDeviceName.c_str(), newStatus);
            return hardware::Void();
        }
        // no lock needed since listener is set up only once during
        // CameraProviderManager initialization and then never changed till it is
        // destructed.
        listener = mManager->getStatusListener();
     }
    // Call without lock held to allow reentrancy into provider manager
    // The problem with holding mLock here is that we
    // might be limiting re-entrancy : CameraService::onTorchStatusChanged calls
    // back into CameraProviderManager which might try to hold mLock again (eg:
    // findDeviceInfo, which should be holding mLock while iterating through
    // each provider's devices).
    if (listener != nullptr) {
        listener->onTorchStatusChanged(String8(id.c_str()),
                hidlToInternalTorchModeStatus(newStatus), systemCameraKind);
    }
    return hardware::Void();
}

void HidlProviderInfo::serviceDied(uint64_t cookie,
        const wp<hidl::base::V1_0::IBase>& who) {
    (void) who;
    ALOGI("Camera provider '%s' has died; removing it", mProviderInstance.c_str());
    if (cookie != mId) {
        ALOGW("%s: Unexpected serviceDied cookie %" PRIu64 ", expected %" PRIu32,
                __FUNCTION__, cookie, mId);
    }
    mManager->removeProvider(mProviderInstance);
}

std::unique_ptr<CameraProviderManager::ProviderInfo::DeviceInfo>
    HidlProviderInfo::initializeDeviceInfo(
        const std::string &name, const metadata_vendor_id_t tagId,
        const std::string &id, uint16_t minorVersion) {
    Status status;

    auto cameraInterface = startDeviceInterface(name);
    if (cameraInterface == nullptr) return nullptr;

    common::V1_0::CameraResourceCost resourceCost;
    cameraInterface->getResourceCost([&status, &resourceCost](
        Status s, common::V1_0::CameraResourceCost cost) {
                status = s;
                resourceCost = cost;
            });
    if (status != Status::OK) {
        ALOGE("%s: Unable to obtain resource costs for camera device %s: %s", __FUNCTION__,
                name.c_str(), statusToString(status));
        return nullptr;
    }

    for (auto& conflictName : resourceCost.conflictingDevices) {
        uint16_t major, minor;
        std::string type, id;
        status_t res = parseDeviceName(conflictName, &major, &minor, &type, &id);
        if (res != OK) {
            ALOGE("%s: Failed to parse conflicting device %s", __FUNCTION__, conflictName.c_str());
            return nullptr;
        }
        conflictName = id;
    }

    return std::unique_ptr<DeviceInfo3>(
        new HidlDeviceInfo3(name, tagId, id, minorVersion, hidlToInternalResourceCost(resourceCost),
                this, mProviderPublicCameraIds, cameraInterface));
}

status_t HidlProviderInfo::reCacheConcurrentStreamingCameraIdsLocked() {
    if (mMinorVersion < 6) {
      // Unsupported operation, nothing to do here
      return OK;
    }
    // Check if the provider is currently active - not going to start it up for this notification
    auto interface = mSavedInterface != nullptr ? mSavedInterface : mActiveInterface.promote();
    if (interface == nullptr) {
        ALOGE("%s: camera provider interface for %s is not valid", __FUNCTION__,
                mProviderName.c_str());
        return INVALID_OPERATION;
    }
    auto castResult = provider::V2_6::ICameraProvider::castFrom(interface);

    if (castResult.isOk()) {
        sp<provider::V2_6::ICameraProvider> interface2_6 = castResult;
        if (interface2_6 != nullptr) {
            return getConcurrentCameraIdsInternalLocked(interface2_6);
        } else {
            // This should not happen since mMinorVersion >= 6
            ALOGE("%s: mMinorVersion was >= 6, but interface2_6 was nullptr", __FUNCTION__);
            return UNKNOWN_ERROR;
        }
    }
    return OK;
}

status_t HidlProviderInfo::getConcurrentCameraIdsInternalLocked(
        sp<provider::V2_6::ICameraProvider> &interface2_6) {
    if (interface2_6 == nullptr) {
        ALOGE("%s: null interface provided", __FUNCTION__);
        return BAD_VALUE;
    }
    Status status = Status::OK;
    hardware::Return<void> ret =
            interface2_6->getConcurrentStreamingCameraIds([&status, this](
            Status concurrentIdStatus, // TODO: Move all instances of hidl_string to 'using'
            const hardware::hidl_vec<hardware::hidl_vec<hardware::hidl_string>>&
                        cameraDeviceIdCombinations) {
            status = concurrentIdStatus;
            if (status == Status::OK) {
                mConcurrentCameraIdCombinations.clear();
                for (auto& combination : cameraDeviceIdCombinations) {
                    std::unordered_set<std::string> deviceIds;
                    for (auto &cameraDeviceId : combination) {
                        deviceIds.insert(cameraDeviceId.c_str());
                    }
                    mConcurrentCameraIdCombinations.push_back(std::move(deviceIds));
                }
            } });
    if (!ret.isOk()) {
        ALOGE("%s: Transaction error in getting concurrent camera ID list from provider '%s'",
                __FUNCTION__, mProviderName.c_str());
            return DEAD_OBJECT;
    }
    if (status != Status::OK) {
        ALOGE("%s: Unable to query for camera devices from provider '%s'",
                    __FUNCTION__, mProviderName.c_str());
        return mapToStatusT(status);
    }
    return OK;
}

HidlProviderInfo::HidlDeviceInfo3::HidlDeviceInfo3(
        const std::string& name,
        const metadata_vendor_id_t tagId,
        const std::string &id, uint16_t minorVersion,
        const CameraResourceCost& resourceCost,
        sp<CameraProviderManager::ProviderInfo> parentProvider,
        const std::vector<std::string>& publicCameraIds,
        sp<hardware::camera::device::V3_2::ICameraDevice> interface) :
        DeviceInfo3(name, tagId, id, minorVersion, resourceCost, parentProvider, publicCameraIds) {

    // Get camera characteristics and initialize flash unit availability
    Status status;
    hardware::Return<void> ret;
    ret = interface->getCameraCharacteristics([&status, this](Status s,
                    device::V3_2::CameraMetadata metadata) {
                status = s;
                if (s == Status::OK) {
                    camera_metadata_t *buffer =
                            reinterpret_cast<camera_metadata_t*>(metadata.data());
                    size_t expectedSize = metadata.size();
                    int res = validate_camera_metadata_structure(buffer, &expectedSize);
                    if (res == OK || res == CAMERA_METADATA_VALIDATION_SHIFTED) {
                        set_camera_metadata_vendor_id(buffer, mProviderTagid);
                        mCameraCharacteristics = buffer;
                    } else {
                        ALOGE("%s: Malformed camera metadata received from HAL", __FUNCTION__);
                        status = Status::INTERNAL_ERROR;
                    }
                }
            });
    if (!ret.isOk()) {
        ALOGE("%s: Transaction error getting camera characteristics for device %s"
                " to check for a flash unit: %s", __FUNCTION__, id.c_str(),
                ret.description().c_str());
        return;
    }
    if (status != Status::OK) {
        ALOGE("%s: Unable to get camera characteristics for device %s: %s (%d)",
                __FUNCTION__, id.c_str(), statusToString(status), status);
        return;
    }

    if (mCameraCharacteristics.exists(ANDROID_INFO_DEVICE_STATE_ORIENTATIONS)) {
        const auto &stateMap = mCameraCharacteristics.find(ANDROID_INFO_DEVICE_STATE_ORIENTATIONS);
        if ((stateMap.count > 0) && ((stateMap.count % 2) == 0)) {
            for (size_t i = 0; i < stateMap.count; i += 2) {
                mDeviceStateOrientationMap.emplace(stateMap.data.i64[i], stateMap.data.i64[i+1]);
            }
        } else {
            ALOGW("%s: Invalid ANDROID_INFO_DEVICE_STATE_ORIENTATIONS map size: %zu", __FUNCTION__,
                    stateMap.count);
        }
    }

    mSystemCameraKind = getSystemCameraKind();

    status_t res = fixupMonochromeTags();
    if (OK != res) {
        ALOGE("%s: Unable to fix up monochrome tags based for older HAL version: %s (%d)",
                __FUNCTION__, strerror(-res), res);
        return;
    }
    auto stat = addDynamicDepthTags();
    if (OK != stat) {
        ALOGE("%s: Failed appending dynamic depth tags: %s (%d)", __FUNCTION__, strerror(-stat),
                stat);
    }
    res = deriveHeicTags();
    if (OK != res) {
        ALOGE("%s: Unable to derive HEIC tags based on camera and media capabilities: %s (%d)",
                __FUNCTION__, strerror(-res), res);
    }

    if (SessionConfigurationUtils::isUltraHighResolutionSensor(mCameraCharacteristics)) {
        status_t status = addDynamicDepthTags(/*maxResolution*/true);
        if (OK != status) {
            ALOGE("%s: Failed appending dynamic depth tags for maximum resolution mode: %s (%d)",
                    __FUNCTION__, strerror(-status), status);
        }

        status = deriveHeicTags(/*maxResolution*/true);
        if (OK != status) {
            ALOGE("%s: Unable to derive HEIC tags based on camera and media capabilities for"
                    "maximum resolution mode: %s (%d)", __FUNCTION__, strerror(-status), status);
        }
    }

    res = addRotateCropTags();
    if (OK != res) {
        ALOGE("%s: Unable to add default SCALER_ROTATE_AND_CROP tags: %s (%d)", __FUNCTION__,
                strerror(-res), res);
    }
    res = addPreCorrectionActiveArraySize();
    if (OK != res) {
        ALOGE("%s: Unable to add PRE_CORRECTION_ACTIVE_ARRAY_SIZE: %s (%d)", __FUNCTION__,
                strerror(-res), res);
    }
    res = camera3::ZoomRatioMapper::overrideZoomRatioTags(
            &mCameraCharacteristics, &mSupportNativeZoomRatio);
    if (OK != res) {
        ALOGE("%s: Unable to override zoomRatio related tags: %s (%d)",
                __FUNCTION__, strerror(-res), res);
    }

    camera_metadata_entry flashAvailable =
            mCameraCharacteristics.find(ANDROID_FLASH_INFO_AVAILABLE);
    if (flashAvailable.count == 1 &&
            flashAvailable.data.u8[0] == ANDROID_FLASH_INFO_AVAILABLE_TRUE) {
        mHasFlashUnit = true;
    } else {
        mHasFlashUnit = false;
    }

    camera_metadata_entry entry =
            mCameraCharacteristics.find(ANDROID_FLASH_INFO_STRENGTH_DEFAULT_LEVEL);
    if (entry.count == 1) {
        mTorchDefaultStrengthLevel = entry.data.i32[0];
    } else {
        mTorchDefaultStrengthLevel = 0;
    }
    entry = mCameraCharacteristics.find(ANDROID_FLASH_INFO_STRENGTH_MAXIMUM_LEVEL);
    if (entry.count == 1) {
        mTorchMaximumStrengthLevel = entry.data.i32[0];
    } else {
        mTorchMaximumStrengthLevel = 0;
    }

    mTorchStrengthLevel = 0;

    queryPhysicalCameraIds();

    // Get physical camera characteristics if applicable
    auto castResult = device::V3_5::ICameraDevice::castFrom(interface);
    if (!castResult.isOk()) {
        ALOGV("%s: Unable to convert ICameraDevice instance to version 3.5", __FUNCTION__);
        return;
    }
    sp<device::V3_5::ICameraDevice> interface_3_5 = castResult;
    if (interface_3_5 == nullptr) {
        ALOGE("%s: Converted ICameraDevice instance to nullptr", __FUNCTION__);
        return;
    }

    if (mIsLogicalCamera) {
        for (auto& id : mPhysicalIds) {
            if (std::find(mPublicCameraIds.begin(), mPublicCameraIds.end(), id) !=
                    mPublicCameraIds.end()) {
                continue;
            }

            hardware::hidl_string hidlId(id);
            ret = interface_3_5->getPhysicalCameraCharacteristics(hidlId,
                    [&status, &id, this](Status s, device::V3_2::CameraMetadata metadata) {
                status = s;
                if (s == Status::OK) {
                    camera_metadata_t *buffer =
                            reinterpret_cast<camera_metadata_t*>(metadata.data());
                    size_t expectedSize = metadata.size();
                    int res = validate_camera_metadata_structure(buffer, &expectedSize);
                    if (res == OK || res == CAMERA_METADATA_VALIDATION_SHIFTED) {
                        set_camera_metadata_vendor_id(buffer, mProviderTagid);
                        mPhysicalCameraCharacteristics[id] = buffer;
                    } else {
                        ALOGE("%s: Malformed camera metadata received from HAL", __FUNCTION__);
                        status = Status::INTERNAL_ERROR;
                    }
                }
            });

            if (!ret.isOk()) {
                ALOGE("%s: Transaction error getting physical camera %s characteristics for %s: %s",
                        __FUNCTION__, id.c_str(), id.c_str(), ret.description().c_str());
                return;
            }
            if (status != Status::OK) {
                ALOGE("%s: Unable to get physical camera %s characteristics for device %s: %s (%d)",
                        __FUNCTION__, id.c_str(), mId.c_str(),
                        statusToString(status), status);
                return;
            }

            res = camera3::ZoomRatioMapper::overrideZoomRatioTags(
                    &mPhysicalCameraCharacteristics[id], &mSupportNativeZoomRatio);
            if (OK != res) {
                ALOGE("%s: Unable to override zoomRatio related tags: %s (%d)",
                        __FUNCTION__, strerror(-res), res);
            }
        }
    }

    if (!kEnableLazyHal) {
        // Save HAL reference indefinitely
        mSavedInterface = interface;
    }


}

status_t HidlProviderInfo::HidlDeviceInfo3::setTorchMode(bool enabled) {
    using hardware::camera::common::V1_0::TorchMode;
    const sp<hardware::camera::device::V3_2::ICameraDevice> interface = startDeviceInterface();
    Status s = interface->setTorchMode(enabled ? TorchMode::ON : TorchMode::OFF);
    return mapToStatusT(s);
}

status_t HidlProviderInfo::HidlDeviceInfo3::turnOnTorchWithStrengthLevel(
        int32_t torchStrength) {
    const sp<hardware::camera::device::V3_2::ICameraDevice> interface = startDeviceInterface();
    if (interface == nullptr) {
        return DEAD_OBJECT;
    }
    sp<hardware::camera::device::V3_8::ICameraDevice> interface_3_8 = nullptr;
    auto castResult_3_8 = device::V3_8::ICameraDevice::castFrom(interface);
    if (castResult_3_8.isOk()) {
        interface_3_8 = castResult_3_8;
    }

    if (interface_3_8 == nullptr) {
        return INVALID_OPERATION;
    }

    Status s = interface_3_8->turnOnTorchWithStrengthLevel(torchStrength);
    if (s == Status::OK) {
        mTorchStrengthLevel = torchStrength;
    }
    return mapToStatusT(s);
}

status_t HidlProviderInfo::HidlDeviceInfo3::getTorchStrengthLevel(int32_t *torchStrength) {
    if (torchStrength == nullptr) {
        return BAD_VALUE;
    }
    const sp<hardware::camera::device::V3_2::ICameraDevice> interface = startDeviceInterface();
    if (interface == nullptr) {
        return DEAD_OBJECT;
    }
    auto castResult_3_8 = device::V3_8::ICameraDevice::castFrom(interface);
    sp<hardware::camera::device::V3_8::ICameraDevice> interface_3_8 = nullptr;
    if (castResult_3_8.isOk()) {
        interface_3_8 = castResult_3_8;
    }

    if (interface_3_8 == nullptr) {
        return INVALID_OPERATION;
    }

    Status callStatus;
    status_t res;
    hardware::Return<void> ret = interface_3_8->getTorchStrengthLevel([&callStatus, &torchStrength]
        (Status status, const int32_t& torchStrengthLevel) {
        callStatus = status;
        if (status == Status::OK) {
             *torchStrength = torchStrengthLevel;
        } });

    if (ret.isOk()) {
        switch (callStatus) {
            case Status::OK:
                // Expected case, do nothing.
                res = OK;
                break;
            case Status::METHOD_NOT_SUPPORTED:
                res = INVALID_OPERATION;
                break;
            default:
                ALOGE("%s: Get torch strength level failed: %d", __FUNCTION__, callStatus);
                res = UNKNOWN_ERROR;
        }
    } else {
        ALOGE("%s: Unexpected binder error: %s", __FUNCTION__, ret.description().c_str());
        res = UNKNOWN_ERROR;
    }

    return res;
}

sp<hardware::camera::device::V3_2::ICameraDevice>
HidlProviderInfo::HidlDeviceInfo3::startDeviceInterface() {
    Mutex::Autolock l(mDeviceAvailableLock);
    sp<hardware::camera::device::V3_2::ICameraDevice> device;
    ATRACE_CALL();
    if (mSavedInterface == nullptr) {
        sp<HidlProviderInfo> parentProvider =
                static_cast<HidlProviderInfo *>(mParentProvider.promote().get());
        if (parentProvider != nullptr) {
            // Wait for lazy HALs to confirm device availability
            if (parentProvider->isExternalLazyHAL() && !mIsDeviceAvailable) {
                ALOGV("%s: Wait for external device to become available %s",
                      __FUNCTION__,
                      mId.c_str());

                auto res = mDeviceAvailableSignal.waitRelative(mDeviceAvailableLock,
                                                         kDeviceAvailableTimeout);
                if (res != OK) {
                    ALOGE("%s: Failed waiting for device to become available",
                          __FUNCTION__);
                    return nullptr;
                }
            }

            device = parentProvider->startDeviceInterface(mName);
        }
    } else {
        device = (hardware::camera::device::V3_2::ICameraDevice *) mSavedInterface.get();
    }
    return device;
}

status_t HidlProviderInfo::HidlDeviceInfo3::dumpState(int fd) {
    native_handle_t* handle = native_handle_create(1,0);
    handle->data[0] = fd;
    const sp<hardware::camera::device::V3_2::ICameraDevice> interface =
            startDeviceInterface();
    if (interface == nullptr) {
        return DEAD_OBJECT;
    }
    auto ret = interface->dumpState(handle);
    native_handle_delete(handle);
    if (!ret.isOk()) {
        return INVALID_OPERATION;
    }
    return OK;
}

status_t HidlProviderInfo::HidlDeviceInfo3::isSessionConfigurationSupported(
        const SessionConfiguration &configuration, bool overrideForPerfClass, bool *status) {

    hardware::camera::device::V3_8::StreamConfiguration streamConfiguration;
    bool earlyExit = false;
    camera3::metadataGetter getMetadata = [this](const String8 &id, bool /*overrideForPerfClass*/) {
          CameraMetadata physicalChars;
          getPhysicalCameraCharacteristics(id.c_str(), &physicalChars);
          return physicalChars;
    };
    auto bRes = SessionConfigurationUtils::convertToHALStreamCombination(configuration,
            String8(mId.c_str()), mCameraCharacteristics, getMetadata, mPhysicalIds,
            streamConfiguration, overrideForPerfClass, &earlyExit);

    if (!bRes.isOk()) {
        return UNKNOWN_ERROR;
    }

    if (earlyExit) {
        *status = false;
        return OK;
    }

    const sp<hardware::camera::device::V3_2::ICameraDevice> interface =
            startDeviceInterface();

    if (interface == nullptr) {
        return DEAD_OBJECT;
    }

    auto castResult_3_5 = device::V3_5::ICameraDevice::castFrom(interface);
    sp<hardware::camera::device::V3_5::ICameraDevice> interface_3_5 = castResult_3_5;
    auto castResult_3_7 = device::V3_7::ICameraDevice::castFrom(interface);
    sp<hardware::camera::device::V3_7::ICameraDevice> interface_3_7 = castResult_3_7;
    auto castResult_3_8 = device::V3_8::ICameraDevice::castFrom(interface);
    sp<hardware::camera::device::V3_8::ICameraDevice> interface_3_8 = castResult_3_8;

    status_t res;
    Status callStatus;
    ::android::hardware::Return<void> ret;
    auto halCb =
            [&callStatus, &status] (Status s, bool combStatus) {
                callStatus = s;
                *status = combStatus;
            };
    if (interface_3_8 != nullptr) {
        ret = interface_3_8->isStreamCombinationSupported_3_8(streamConfiguration, halCb);
    } else if (interface_3_7 != nullptr) {
        hardware::camera::device::V3_7::StreamConfiguration configuration_3_7;
        bool success = SessionConfigurationUtils::convertHALStreamCombinationFromV38ToV37(
                configuration_3_7, streamConfiguration);
        if (!success) {
            *status = false;
            return OK;
        }
        ret = interface_3_7->isStreamCombinationSupported_3_7(configuration_3_7, halCb);
    } else if (interface_3_5 != nullptr) {
        hardware::camera::device::V3_7::StreamConfiguration configuration_3_7;
        bool success = SessionConfigurationUtils::convertHALStreamCombinationFromV38ToV37(
                configuration_3_7, streamConfiguration);
        if (!success) {
            *status = false;
            return OK;
        }
        hardware::camera::device::V3_4::StreamConfiguration configuration_3_4;
        success = SessionConfigurationUtils::convertHALStreamCombinationFromV37ToV34(
                configuration_3_4, configuration_3_7);
        if (!success) {
            *status = false;
            return OK;
        }
        ret = interface_3_5->isStreamCombinationSupported(configuration_3_4, halCb);
    } else {
        return INVALID_OPERATION;
    }
    if (ret.isOk()) {
        switch (callStatus) {
            case Status::OK:
                // Expected case, do nothing.
                res = OK;
                break;
            case Status::METHOD_NOT_SUPPORTED:
                res = INVALID_OPERATION;
                break;
            default:
                ALOGE("%s: Session configuration query failed: %d", __FUNCTION__, callStatus);
                res = UNKNOWN_ERROR;
        }
    } else {
        ALOGE("%s: Unexpected binder error: %s", __FUNCTION__, ret.description().c_str());
        res = UNKNOWN_ERROR;
    }

    return res;
}

status_t HidlProviderInfo::convertToHALStreamCombinationAndCameraIdsLocked(
        const std::vector<CameraIdAndSessionConfiguration> &cameraIdsAndSessionConfigs,
        const std::set<std::string>& perfClassPrimaryCameraIds,
        int targetSdkVersion,
        hardware::hidl_vec<CameraIdAndStreamCombination> *halCameraIdsAndStreamCombinations,
        bool *earlyExit) {
    binder::Status bStatus = binder::Status::ok();
    std::vector<CameraIdAndStreamCombination> halCameraIdsAndStreamsV;
    bool shouldExit = false;
    status_t res = OK;
    for (auto &cameraIdAndSessionConfig : cameraIdsAndSessionConfigs) {
        const std::string& cameraId = cameraIdAndSessionConfig.mCameraId;
        hardware::camera::device::V3_8::StreamConfiguration streamConfiguration;
        CameraMetadata deviceInfo;
        bool overrideForPerfClass =
                SessionConfigurationUtils::targetPerfClassPrimaryCamera(
                        perfClassPrimaryCameraIds, cameraId, targetSdkVersion);
        res = mManager->getCameraCharacteristicsLocked(cameraId, overrideForPerfClass, &deviceInfo);
        if (res != OK) {
            return res;
        }
        camera3::metadataGetter getMetadata =
                [this](const String8 &id, bool overrideForPerfClass) {
                    CameraMetadata physicalDeviceInfo;
                    mManager->getCameraCharacteristicsLocked(id.string(), overrideForPerfClass,
                                                   &physicalDeviceInfo);
                    return physicalDeviceInfo;
                };
        std::vector<std::string> physicalCameraIds;
        mManager->isLogicalCameraLocked(cameraId, &physicalCameraIds);
        bStatus =
            SessionConfigurationUtils::convertToHALStreamCombination(
                    cameraIdAndSessionConfig.mSessionConfiguration,
                    String8(cameraId.c_str()), deviceInfo, getMetadata,
                    physicalCameraIds, streamConfiguration,
                    overrideForPerfClass, &shouldExit);
        if (!bStatus.isOk()) {
            ALOGE("%s: convertToHALStreamCombination failed", __FUNCTION__);
            return INVALID_OPERATION;
        }
        if (shouldExit) {
            *earlyExit = true;
            return OK;
        }
        CameraIdAndStreamCombination halCameraIdAndStream;
        halCameraIdAndStream.cameraId = cameraId;
        SessionConfigurationUtils::convertHALStreamCombinationFromV38ToV37(
                halCameraIdAndStream.streamConfiguration, streamConfiguration);
        halCameraIdsAndStreamsV.push_back(halCameraIdAndStream);
    }
    *halCameraIdsAndStreamCombinations = halCameraIdsAndStreamsV;
    return OK;
}

status_t HidlProviderInfo::isConcurrentSessionConfigurationSupported(
        const std::vector<CameraIdAndSessionConfiguration> &cameraIdsAndSessionConfigs,
        const std::set<std::string>& perfClassPrimaryCameraIds,
        int targetSdkVersion, bool *isSupported) {

      hardware::hidl_vec<CameraIdAndStreamCombination> halCameraIdsAndStreamCombinations;
      bool knowUnsupported = false;
      status_t res = convertToHALStreamCombinationAndCameraIdsLocked(
              cameraIdsAndSessionConfigs, perfClassPrimaryCameraIds,
              targetSdkVersion, &halCameraIdsAndStreamCombinations, &knowUnsupported);
      if (res != OK) {
          ALOGE("%s unable to convert session configurations provided to HAL stream"
                "combinations", __FUNCTION__);
          return res;
      }
      if (knowUnsupported) {
          // We got to know the streams aren't valid before doing the HAL
          // call itself.
          *isSupported = false;
          return OK;
      }

    if (mMinorVersion >= 6) {
        // Check if the provider is currently active - not going to start it for this notification
        auto interface = mSavedInterface != nullptr ? mSavedInterface : mActiveInterface.promote();
        if (interface == nullptr) {
            // TODO: This might be some other problem
            return INVALID_OPERATION;
        }
        auto castResult2_6 = provider::V2_6::ICameraProvider::castFrom(interface);
        auto castResult2_7 = provider::V2_7::ICameraProvider::castFrom(interface);
        Status callStatus;
        auto cb =
                [&isSupported, &callStatus](Status s, bool supported) {
                      callStatus = s;
                      *isSupported = supported; };

        ::android::hardware::Return<void> ret;
        sp<provider::V2_7::ICameraProvider> interface_2_7;
        sp<provider::V2_6::ICameraProvider> interface_2_6;
        if (mMinorVersion >= 7 && castResult2_7.isOk()) {
            interface_2_7 = castResult2_7;
            if (interface_2_7 != nullptr) {
                ret = interface_2_7->isConcurrentStreamCombinationSupported_2_7(
                        halCameraIdsAndStreamCombinations, cb);
            }
        } else if (mMinorVersion == 6 && castResult2_6.isOk()) {
            interface_2_6 = castResult2_6;
            if (interface_2_6 != nullptr) {
                hardware::hidl_vec<provider::V2_6::CameraIdAndStreamCombination>
                        halCameraIdsAndStreamCombinations_2_6;
                size_t numStreams = halCameraIdsAndStreamCombinations.size();
                halCameraIdsAndStreamCombinations_2_6.resize(numStreams);
                for (size_t i = 0; i < numStreams; i++) {
                    using namespace camera3;
                    auto const& combination = halCameraIdsAndStreamCombinations[i];
                    halCameraIdsAndStreamCombinations_2_6[i].cameraId = combination.cameraId;
                    bool success =
                            SessionConfigurationUtils::convertHALStreamCombinationFromV37ToV34(
                                    halCameraIdsAndStreamCombinations_2_6[i].streamConfiguration,
                                    combination.streamConfiguration);
                    if (!success) {
                        *isSupported = false;
                        return OK;
                    }
                }
                ret = interface_2_6->isConcurrentStreamCombinationSupported(
                        halCameraIdsAndStreamCombinations_2_6, cb);
            }
        }

        if (interface_2_7 != nullptr || interface_2_6 != nullptr) {
            if (ret.isOk()) {
                switch (callStatus) {
                    case Status::OK:
                        // Expected case, do nothing.
                        res = OK;
                        break;
                    case Status::METHOD_NOT_SUPPORTED:
                        res = INVALID_OPERATION;
                        break;
                    default:
                        ALOGE("%s: Session configuration query failed: %d", __FUNCTION__,
                                  callStatus);
                        res = UNKNOWN_ERROR;
                }
            } else {
                ALOGE("%s: Unexpected binder error: %s", __FUNCTION__, ret.description().c_str());
                res = UNKNOWN_ERROR;
            }
            return res;
        }
    }
    // unsupported operation
    return INVALID_OPERATION;
}

status_t HidlVendorTagDescriptor::createDescriptorFromHidl(
        const hardware::hidl_vec<common::V1_0::VendorTagSection>& vts,
        sp<VendorTagDescriptor>& descriptor) {

    int tagCount = 0;

    for (size_t s = 0; s < vts.size(); s++) {
        tagCount += vts[s].tags.size();
    }

    if (tagCount < 0 || tagCount > INT32_MAX) {
        ALOGE("%s: tag count %d from vendor tag sections is invalid.", __FUNCTION__, tagCount);
        return BAD_VALUE;
    }

    Vector<uint32_t> tagArray;
    LOG_ALWAYS_FATAL_IF(tagArray.resize(tagCount) != tagCount,
            "%s: too many (%u) vendor tags defined.", __FUNCTION__, tagCount);


    sp<HidlVendorTagDescriptor> desc = new HidlVendorTagDescriptor();
    desc->mTagCount = tagCount;

    SortedVector<String8> sections;
    KeyedVector<uint32_t, String8> tagToSectionMap;

    int idx = 0;
    for (size_t s = 0; s < vts.size(); s++) {
        const common::V1_0::VendorTagSection& section = vts[s];
        const char *sectionName = section.sectionName.c_str();
        if (sectionName == NULL) {
            ALOGE("%s: no section name defined for vendor tag section %zu.", __FUNCTION__, s);
            return BAD_VALUE;
        }
        String8 sectionString(sectionName);
        sections.add(sectionString);

        for (size_t j = 0; j < section.tags.size(); j++) {
            uint32_t tag = section.tags[j].tagId;
            if (tag < CAMERA_METADATA_VENDOR_TAG_BOUNDARY) {
                ALOGE("%s: vendor tag %d not in vendor tag section.", __FUNCTION__, tag);
                return BAD_VALUE;
            }

            tagArray.editItemAt(idx++) = section.tags[j].tagId;

            const char *tagName = section.tags[j].tagName.c_str();
            if (tagName == NULL) {
                ALOGE("%s: no tag name defined for vendor tag %d.", __FUNCTION__, tag);
                return BAD_VALUE;
            }
            desc->mTagToNameMap.add(tag, String8(tagName));
            tagToSectionMap.add(tag, sectionString);

            int tagType = (int) section.tags[j].tagType;
            if (tagType < 0 || tagType >= NUM_TYPES) {
                ALOGE("%s: tag type %d from vendor ops does not exist.", __FUNCTION__, tagType);
                return BAD_VALUE;
            }
            desc->mTagToTypeMap.add(tag, tagType);
        }
    }

    desc->mSections = sections;

    for (size_t i = 0; i < tagArray.size(); ++i) {
        uint32_t tag = tagArray[i];
        String8 sectionString = tagToSectionMap.valueFor(tag);

        // Set up tag to section index map
        ssize_t index = sections.indexOf(sectionString);
        LOG_ALWAYS_FATAL_IF(index < 0, "index %zd must be non-negative", index);
        desc->mTagToSectionMap.add(tag, static_cast<uint32_t>(index));

        // Set up reverse mapping
        ssize_t reverseIndex = -1;
        if ((reverseIndex = desc->mReverseMapping.indexOfKey(sectionString)) < 0) {
            KeyedVector<String8, uint32_t>* nameMapper = new KeyedVector<String8, uint32_t>();
            reverseIndex = desc->mReverseMapping.add(sectionString, nameMapper);
        }
        desc->mReverseMapping[reverseIndex]->add(desc->mTagToNameMap.valueFor(tag), tag);
    }

    descriptor = std::move(desc);
    return OK;
}

} //namespace android