/* Copyright (c) 2023 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
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

bool HostResolverManager::Job::MaybeDropBlockedOnionTasks() {
  if (!IsOnionHostnameResolutionBlocked(key_.resolve_context.get(),
                                        key_.host.GetHostname())) {
    return false;
  }
  return MaybeServeBlockedOnionJobFromHosts(DropOnionNetworkTasks());
}

bool HostResolverManager::Job::MaybeDeferBlockedOnionJob() {
  // Only the first slot of a job and the extra slots of a running DNS task
  // start work; leave any other grant to Start().
  if ((num_occupied_job_slots_ != 0 && !dns_task_) ||
      !IsOnionHostnameResolutionBlocked(key_.resolve_context.get(),
                                        key_.host.GetHostname())) {
    return false;
  }

  // Take the slot as Start() would, then hand any extra slots back by stopping
  // the DNS task. Marking the job as running makes Finish() release the
  // remaining slot.
  handle_.Reset();
  ++num_occupied_job_slots_;
  KillDnsTask();
  job_running_ = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&Job::ResumeBlockedOnionJob,
                     weak_ptr_factory_.GetWeakPtr(), DropOnionNetworkTasks()));
  return true;
}

void HostResolverManager::Job::ResumeBlockedOnionJob(bool dropped_system_task) {
  if (MaybeServeBlockedOnionJobFromHosts(dropped_system_task)) {
    return;
  }
  // Runs the remaining cache lookups, or fails the job.
  RunNextTask();
}

bool HostResolverManager::Job::DropOnionNetworkTasks() {
  const bool dropped_system_task =
      std::ranges::contains(tasks_, TaskType::SYSTEM);
  std::erase_if(tasks_, [](TaskType task) { return !IsLocalTask(task); });
  return dropped_system_task;
}

bool HostResolverManager::Job::MaybeServeBlockedOnionJobFromHosts(
    bool dropped_system_task) {
  if (!dropped_system_task || !resolver_) {
    return false;
  }
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
