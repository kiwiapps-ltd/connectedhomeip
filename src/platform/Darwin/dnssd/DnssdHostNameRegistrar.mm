/*
 *
 *    Copyright (c) 2022 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#if !__has_feature(objc_arc)
#error This file must be compiled with ARC. Use -fobjc-arc flag (or convert project to ARC).
#endif

#include "DnssdHostNameRegistrar.h"
#include "DnssdError.h"
#include "DnssdImpl.h"

#include <algorithm>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/ethernet.h>
#include <net/if_dl.h>
#include <netdb.h>
#include <stdio.h>

#include <set>

#include <platform/CHIPDeviceLayer.h>

constexpr DNSServiceFlags kRegisterRecordFlags = kDNSServiceFlagsShared;
constexpr uint16_t kRegisterRecordTimeoutInSeconds = 30;

namespace chip {
namespace Dnssd {

    namespace {
        // MatterBridge patch — see DnssdHostNameRegistrar.h for the full
        // rationale. Builds a deterministic byte-level signature of the
        // resolved interface+address set so we can detect no-op
        // nw_path_monitor updates (which fire many times per minute on a
        // stable LAN due to BSSID transitions / IPv6 SLAAC refreshes) and
        // skip the DNSServiceRefDeallocate + DNSServiceRegisterRecord
        // cycle. Without this, mDNSResponder accumulates per-record state
        // until the system OOMs.
        std::string SignatureForInterfaces(Inet::Darwin::InetInterfacesVector const & v4,
                                           Inet::Darwin::Inet6InterfacesVector const & v6)
        {
            std::vector<std::string> entries;
            entries.reserve(v4.size() + v6.size());
            char buf[80];
            for (auto const & p : v4) {
                snprintf(buf, sizeof(buf), "%u:4:%08x",
                    nw_interface_get_index(p.first),
                    static_cast<unsigned>(p.second.s_addr));
                entries.emplace_back(buf);
            }
            for (auto const & p : v6) {
                auto const & a = p.second;
                snprintf(buf, sizeof(buf),
                    "%u:6:%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                    nw_interface_get_index(p.first),
                    a.s6_addr[0], a.s6_addr[1], a.s6_addr[2], a.s6_addr[3],
                    a.s6_addr[4], a.s6_addr[5], a.s6_addr[6], a.s6_addr[7],
                    a.s6_addr[8], a.s6_addr[9], a.s6_addr[10], a.s6_addr[11],
                    a.s6_addr[12], a.s6_addr[13], a.s6_addr[14], a.s6_addr[15]);
                entries.emplace_back(buf);
            }
            std::sort(entries.begin(), entries.end());
            std::string sig;
            sig.reserve(entries.size() * 32);
            for (auto const & e : entries) {
                sig.append(e);
                sig.push_back(';');
            }
            return sig;
        }
    } // namespace

    HostNameRegistrar::~HostNameRegistrar()
    {
        Unregister();
    }

    DNSServiceErrorType HostNameRegistrar::Init(const char * hostname, Inet::IPAddressType addressType, uint32_t interfaceId)
    {
        mHostname = hostname;
        mServiceRef = nullptr;

        VerifyOrReturnError(CHIP_NO_ERROR == NetworkMonitor::Init(chip::DeviceLayer::PlatformMgrImpl().GetWorkQueue(), addressType, interfaceId),
            kDNSServiceErr_BadState);

        return kDNSServiceErr_NoError;
    }

    CHIP_ERROR HostNameRegistrar::Register(OnRegisterRecordCallback callback)
    {
        VerifyOrReturnError(nullptr != callback, CHIP_ERROR_INVALID_ARGUMENT);

        mOnRegisterRecordCallback = callback;

        CHIP_ERROR error = CHIP_NO_ERROR;

        // If the target interface is kDNSServiceInterfaceIndexLocalOnly, just
        // register the loopback addresses.
        if (IsLocalOnly()) {
            ReturnErrorOnFailure(ResetSharedConnection());

            // Just register the loopback IPv6 address, on mInterfaceId so that the
            // resolution code finds it there.
            auto loopbackIPAddress = Inet::IPAddress::Loopback(Inet::IPAddressType::kIPv6);
            in6_addr loopbackAddr = loopbackIPAddress.ToIPv6();

            error = RegisterInterface(kDNSServiceInterfaceIndexLocalOnly, loopbackAddr, kDNSServiceType_AAAA);
        } else {
            error = StartMonitorInterfaces(
                ^(Inet::Darwin::InetInterfacesVector inetInterfaces, Inet::Darwin::Inet6InterfacesVector inet6Interfaces) {
                    // MatterBridge patch: skip re-register when the resolved
                    // interface+address set is unchanged. nw_path_monitor on
                    // macOS 26.3 fires every 1-2 s on a stable LAN (BSSID
                    // transitions, IPv6 SLAAC refreshes) and re-registering
                    // identical records leaks per-record state in the system
                    // mDNSResponder daemon (verified: 23 GB system OOM after
                    // 7 h with 57 k OnRegisterRecord events).
                    auto signature = SignatureForInterfaces(inetInterfaces, inet6Interfaces);
                    if (signature == mLastInterfacesSignature) {
                        return;
                    }
                    mLastInterfacesSignature = signature;
                    ReturnOnFailure(ResetSharedConnection());
                    RegisterInterfaces(inetInterfaces, kDNSServiceType_A);
                    RegisterInterfaces(inet6Interfaces, kDNSServiceType_AAAA);
                });
        }
        ReturnErrorOnFailure(error);

        auto & systemLayer = static_cast<System::LayerDispatch &>(chip::DeviceLayer::SystemLayer());
        auto delay = System::Clock::Seconds16(kRegisterRecordTimeoutInSeconds);
        return systemLayer.StartTimer(delay, OnRegisterRecordTimeout, this);
    }

    CHIP_ERROR HostNameRegistrar::RegisterInterface(uint32_t interfaceId, uint16_t rtype, const void * rdata, uint16_t rdlen)
    {
        DNSRecordRef dnsRecordRef;
        auto err = DNSServiceRegisterRecord(mServiceRef, &dnsRecordRef, kRegisterRecordFlags, interfaceId, mHostname.c_str(), rtype,
            kDNSServiceClass_IN, rdlen, rdata, 0, OnRegisterRecord, this);
        return Error::ToChipError(err);
    }

    void HostNameRegistrar::Unregister()
    {
        if (!IsLocalOnly()) {
            NetworkMonitor::Stop();
        }
        StopSharedConnection();
        // Clear the dedup cache so a future Register() always re-publishes.
        mLastInterfacesSignature.clear();

        mOnRegisterRecordCallback = nullptr;

        auto & systemLayer = static_cast<System::LayerDispatch &>(chip::DeviceLayer::SystemLayer());
        systemLayer.CancelTimer(OnRegisterRecordTimeout, this);
    }

    CHIP_ERROR HostNameRegistrar::StartSharedConnection()
    {
        VerifyOrReturnError(mServiceRef == nullptr, CHIP_ERROR_INCORRECT_STATE);

        auto err = DNSServiceCreateConnection(&mServiceRef);
        VerifyOrReturnValue(kDNSServiceErr_NoError == err, Error::ToChipError(err));

        err = DNSServiceSetDispatchQueue(mServiceRef, chip::DeviceLayer::PlatformMgrImpl().GetWorkQueue());
        if (kDNSServiceErr_NoError != err) {
            StopSharedConnection();
        }

        return Error::ToChipError(err);
    }

    void HostNameRegistrar::StopSharedConnection()
    {
        if (mServiceRef != nullptr) {
            // All the DNSRecordRefs registered to the shared DNSServiceRef will be deallocated.
            DNSServiceRefDeallocate(mServiceRef);
            mServiceRef = nullptr;
        }
    }

    CHIP_ERROR HostNameRegistrar::ResetSharedConnection()
    {
        StopSharedConnection();
        ReturnLogErrorOnFailure(StartSharedConnection());
        return CHIP_NO_ERROR;
    }

    void HostNameRegistrar::OnRegisterRecord(DNSServiceRef sdRef, DNSRecordRef recordRef, DNSServiceFlags flags, DNSServiceErrorType err,
        void * context)
    {
        ChipLogProgress(Discovery, "Mdns: %s flags: %d", __func__, flags);

        if (kDNSServiceErr_NoError != err) {
            ChipLogError(Discovery, "%s (%s)", __func__, Error::ToString(err));
            return;
        }

        RunOnRegisterRecordCallback(context, err);
    }

    void HostNameRegistrar::OnRegisterRecordTimeout(System::Layer * layer, void * appState)
    {
        RunOnRegisterRecordCallback(appState, kDNSServiceErr_Timeout);
    }

    void HostNameRegistrar::RunOnRegisterRecordCallback(void * context, DNSServiceErrorType error)
    {
        auto * self = static_cast<HostNameRegistrar *>(context);

        auto & systemLayer = static_cast<System::LayerDispatch &>(chip::DeviceLayer::SystemLayer());
        systemLayer.CancelTimer(OnRegisterRecordTimeout, self);

        VerifyOrReturn(nullptr != self->mOnRegisterRecordCallback);

        auto registerRecordCallback = self->mOnRegisterRecordCallback;
        self->mOnRegisterRecordCallback = nullptr;
        registerRecordCallback(error);
    }

} // namespace Dnssd
} // namespace chip
