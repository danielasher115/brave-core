/* Copyright (c) 2023 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/net/dns/onion_hostname_resolution.h"
#include "brave/net/dns/secure_dns_counter.h"

#define BRAVE_RECORD_DNS_COUNTS                                 \
  if (task_type.has_value()) {                                  \
    SecureDnsCounter::GetInstance()->RecordAutoSecureTaskCount( \
        static_cast<int>(*task_type), key_.query_types);        \
  }

#include <net/dns/host_resolver_manager_job.cc>

#undef BRAVE_RECORD_DNS_COUNTS

namespace net {

// Declared by the plaster in host_resolver_manager_job.h and called at the top
// of RunNextTask(), so a job whose context starts blocking .onion after it was
// created, or a bootstrap follow-up, cannot reach the system resolver or DoH.
bool HostResolverManager::Job::MaybeDropBlockedOnionTasks() {
  if (!IsOnionHostnameResolutionBlocked(key_.resolve_context.get(),
                                        key_.host.GetHostname())) {
    return false;
  }

  // Keep the cache lookups, which never leave the machine.
  const bool dropped_system_task =
      std::ranges::contains(tasks_, TaskType::SYSTEM);
  std::erase_if(tasks_, [](TaskType task) { return !IsLocalTask(task); });
  if (!dropped_system_task || !resolver_) {
    return false;
  }

  // ResolveLocally() skips the hosts file when the system resolver runs next,
  // since getaddrinfo() reads it. Read it here instead.
  std::set<std::unique_ptr<HostResolverInternalResult>> results =
      resolver_->ServeFromHosts(
          key_.host.GetHostname(), key_.query_types,
          key_.flags & HOST_RESOLVER_DEFAULT_FAMILY_SET_DUE_TO_NO_IPV6,
          /*tasks=*/{});
  if (std::ranges::none_of(results, [](const auto& result) {
        return result->type() == HostResolverInternalResult::Type::kData;
      })) {
    return false;
  }
  CompleteRequestsWithoutCache(
      HostCache::Entry(results, base::Time::Now(), tick_clock_->NowTicks(),
                       HostCache::Entry::SOURCE_HOSTS),
      /*stale_info=*/std::nullopt, TaskType::SYSTEM);
  return true;
}

}  // namespace net
