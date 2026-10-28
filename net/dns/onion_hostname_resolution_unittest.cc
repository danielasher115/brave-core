/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/test/task_environment.h"
#include "brave/components/tor/buildflags/buildflags.h"
#include "net/base/address_family.h"
#include "net/base/host_port_pair.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/network_handle.h"
#include "net/base/test_completion_callback.h"
#include "net/dns/context_host_resolver.h"
#include "net/dns/dns_config.h"
#include "net/dns/dns_hosts.h"
#include "net/dns/dns_test_util.h"
#include "net/dns/host_resolver.h"
#include "net/dns/host_resolver_manager.h"
#include "net/dns/host_resolver_system_task.h"
#include "net/dns/mock_host_resolver.h"
#include "net/dns/public/dns_over_https_config.h"
#include "net/dns/public/dns_protocol.h"
#include "net/dns/public/secure_dns_mode.h"
#include "net/dns/public/secure_dns_policy.h"
#include "net/log/net_log_with_source.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "net/url_request/url_request_test_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/scheme_host_port.h"

namespace net {

class OnionHostnameResolutionTest : public testing::Test {
 public:
  void SetUp() override {
    host_resolver_proc_ =
        base::MakeRefCounted<RuleBasedHostResolverProc>(nullptr);
    host_resolver_proc_->AddRule("*", "127.0.0.1");
    scoped_host_resolver_proc_.Init(host_resolver_proc_.get());

    auto builder = CreateTestURLRequestContextBuilder();
    builder->set_host_resolver(HostResolver::CreateStandaloneContextResolver(
        /*net_log=*/nullptr, HostResolver::ManagerOptions()));
    context_ = builder->Build();
  }

  int Resolve(std::string_view host,
              std::optional<HostResolver::ResolveHostParameters> parameters =
                  std::nullopt) {
    std::unique_ptr<HostResolver::ResolveHostRequest> request =
        context_->host_resolver()->CreateRequest(
            HostPortPair(host, 80), NetworkAnonymizationKey(),
            handles::kInvalidNetworkHandle, NetLogWithSource(), parameters);
    TestCompletionCallback callback;
    return callback.GetResult(request->Start(callback.callback()));
  }

  int ResolveURL(std::string_view url) {
    std::unique_ptr<HostResolver::ResolveHostRequest> request =
        context_->host_resolver()->CreateRequest(
            url::SchemeHostPort(GURL(url)), NetworkAnonymizationKey(),
            handles::kInvalidNetworkHandle, NetLogWithSource(), std::nullopt);
    TestCompletionCallback callback;
    return callback.GetResult(request->Start(callback.callback()));
  }

  size_t NumSystemResolves() {
    return host_resolver_proc_->NumResolvesForHostPattern("*");
  }

 protected:
  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::IO};
  scoped_refptr<RuleBasedHostResolverProc> host_resolver_proc_;
  ScopedDefaultHostResolverProc scoped_host_resolver_proc_;
  std::unique_ptr<URLRequestContext> context_;
};

#if BUILDFLAG(ENABLE_TOR)

TEST_F(OnionHostnameResolutionTest, BlocksByDefault) {
  EXPECT_EQ(URLRequestContext::TorMode::kDisabled, context_->tor_mode());

  EXPECT_EQ(ERR_NAME_NOT_RESOLVED, Resolve("example.onion"));
  EXPECT_EQ(0u, NumSystemResolves());
}

TEST_F(OnionHostnameResolutionTest, ResolvesOnionInPassthroughMode) {
  context_->set_tor_mode(URLRequestContext::TorMode::kPassthrough);

  EXPECT_EQ(OK, Resolve("example.onion"));
  EXPECT_EQ(1u, NumSystemResolves());
}

TEST_F(OnionHostnameResolutionTest, ResolvesOnionInTorContext) {
  context_->set_tor_mode(URLRequestContext::TorMode::kEnabled);

  EXPECT_EQ(OK, Resolve("example.onion"));
  EXPECT_EQ(1u, NumSystemResolves());
}

TEST_F(OnionHostnameResolutionTest, FailsOnionWhenBlocked) {
  EXPECT_EQ(ERR_NAME_NOT_RESOLVED, Resolve("example.onion"));
  EXPECT_EQ(ERR_NAME_NOT_RESOLVED, Resolve("sub.example.onion"));
  EXPECT_EQ(ERR_NAME_NOT_RESOLVED, Resolve("EXAMPLE.ONION"));
  EXPECT_EQ(ERR_NAME_NOT_RESOLVED, Resolve("example.onion."));
  EXPECT_EQ(ERR_NAME_NOT_RESOLVED, Resolve("onion"));
  EXPECT_EQ(ERR_NAME_NOT_RESOLVED, ResolveURL("https://example.onion/"));
  EXPECT_EQ(ERR_NAME_NOT_RESOLVED, ResolveURL("wss://example.onion/"));
  EXPECT_EQ(0u, NumSystemResolves());
}

TEST_F(OnionHostnameResolutionTest, ResolvesOtherHostsWhenBlocked) {
  EXPECT_EQ(OK, Resolve("example.com"));
  EXPECT_EQ(OK, Resolve("example.onion.com"));
  EXPECT_EQ(OK, Resolve("exampleonion"));
  EXPECT_EQ(OK, ResolveURL("https://example.com/"));
  EXPECT_EQ(4u, NumSystemResolves());
}

TEST_F(OnionHostnameResolutionTest, ServesCachedOnionResultsWhenBlocked) {
  context_->set_tor_mode(URLRequestContext::TorMode::kPassthrough);
  EXPECT_EQ(OK, Resolve("example.onion"));
  EXPECT_EQ(1u, NumSystemResolves());

  context_->set_tor_mode(URLRequestContext::TorMode::kDisabled);
  EXPECT_EQ(OK, Resolve("example.onion"));

  HostResolver::ResolveHostParameters parameters;
  parameters.cache_usage =
      HostResolver::ResolveHostParameters::CacheUsage::DISALLOWED;
  EXPECT_EQ(ERR_NAME_NOT_RESOLVED, Resolve("example.onion", parameters));
  EXPECT_EQ(1u, NumSystemResolves());
}

// Resolves through a MockDnsClient, so lookups can take the built-in DNS and
// DoH paths. The system resolver is `host_resolver_proc_`.
class OnionHostnameResolutionDnsClientTest : public testing::Test {
 public:
  void SetUp() override {
    host_resolver_proc_ =
        base::MakeRefCounted<RuleBasedHostResolverProc>(nullptr);
    host_resolver_proc_->AddRule("*", "127.0.0.1");

    std::unique_ptr<ContextHostResolver> host_resolver =
        HostResolver::CreateStandaloneContextResolver(/*net_log=*/nullptr,
                                                      manager_options_);
    host_resolver->SetHostResolverSystemParamsForTest(
        HostResolverSystemTask::Params(host_resolver_proc_,
                                       /*max_retry_attempts=*/0));
    host_resolver_ = host_resolver.get();

    auto builder = CreateTestURLRequestContextBuilder();
    builder->set_host_resolver(std::move(host_resolver));
    context_ = builder->Build();
  }

  void TearDown() override {
    dns_client_ = nullptr;
    host_resolver_ = nullptr;
  }

  // Uses `secure_dns_mode` with a DoH server, `hosts` as the hosts file and
  // `rules` for DNS and DoH transactions. Insecure built-in DNS is enabled when
  // `insecure_dns` is set; otherwise insecure lookups use the system resolver.
  void SetUpDnsClient(SecureDnsMode secure_dns_mode,
                      MockDnsClientRuleList rules,
                      bool insecure_dns = false,
                      DnsHosts hosts = {}) {
    DnsConfig config({IPEndPoint(IPAddress(192, 168, 1, 1), 53)});
    config.doh_config =
        *DnsOverHttpsConfig::FromString("https://doh.test/dns-query");
    config.secure_dns_mode = secure_dns_mode;
    config.hosts = std::move(hosts);

    auto dns_client =
        std::make_unique<MockDnsClient>(std::move(config), std::move(rules));
    dns_client->set_ignore_system_config_changes(true);
    if (insecure_dns) {
      dns_client->SetInsecureEnabled(InsecureDnsMode::kEnabledBuiltIn,
                                     /*additional_types_enabled=*/false);
    }
    dns_client_ = dns_client.get();
    host_resolver_->GetManagerForTesting()->SetDnsClientForTesting(
        std::move(dns_client));
  }

  std::unique_ptr<HostResolver::ResolveHostRequest> CreateRequest(
      url::SchemeHostPort host,
      SecureDnsPolicy secure_dns_policy = SecureDnsPolicy::kAllow,
      DnsQueryType dns_query_type = DnsQueryType::A) {
    HostResolver::ResolveHostParameters parameters;
    parameters.dns_query_type = dns_query_type;
    parameters.secure_dns_policy = secure_dns_policy;
    return context_->host_resolver()->CreateRequest(
        std::move(host), NetworkAnonymizationKey(),
        handles::kInvalidNetworkHandle, NetLogWithSource(), parameters);
  }

  std::unique_ptr<HostResolver::ResolveHostRequest> CreateRequest(
      std::string_view host,
      DnsQueryType dns_query_type = DnsQueryType::A) {
    return CreateRequest(url::SchemeHostPort(url::kHttpsScheme, host, 443),
                         SecureDnsPolicy::kAllow, dns_query_type);
  }

  size_t NumSystemResolves() {
    return host_resolver_proc_->NumResolvesForHostPattern("*");
  }

 protected:
  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::IO};
  HostResolver::ManagerOptions manager_options_;
  scoped_refptr<RuleBasedHostResolverProc> host_resolver_proc_;
  std::unique_ptr<URLRequestContext> context_;
  raw_ptr<ContextHostResolver> host_resolver_ = nullptr;
  raw_ptr<MockDnsClient> dns_client_ = nullptr;
};

// A lookup queued while .onion was allowed must not go on to DNS after the
// context starts blocking it.
TEST_F(OnionHostnameResolutionDnsClientTest, BlocksQueuedTasksAfterModeChange) {
  MockDnsClientRuleList rules;
  rules.emplace_back(
      "example.onion", dns_protocol::kTypeA, /*secure=*/true,
      MockDnsClientRule::Result(MockDnsClientRule::ResultType::kFail),
      /*delay=*/true);
  rules.emplace_back(
      "example.onion", dns_protocol::kTypeA, /*secure=*/false,
      MockDnsClientRule::Result(MockDnsClientRule::ResultType::kUnexpected),
      /*delay=*/false);
  SetUpDnsClient(SecureDnsMode::kAutomatic, std::move(rules),
                 /*insecure_dns=*/true);
  context_->set_tor_mode(URLRequestContext::TorMode::kPassthrough);

  std::unique_ptr<HostResolver::ResolveHostRequest> request =
      CreateRequest("example.onion");
  TestCompletionCallback callback;
  ASSERT_EQ(ERR_IO_PENDING, request->Start(callback.callback()));

  // The DoH attempt is in flight; its failure would fall back to DNS.
  context_->set_tor_mode(URLRequestContext::TorMode::kDisabled);
  dns_client_->CompleteDelayedTransactions();

  EXPECT_EQ(ERR_NAME_NOT_RESOLVED, callback.WaitForResult());
  EXPECT_EQ(0u, NumSystemResolves());
}

// The DoH follow-up of a bootstrap lookup answered from preset addresses must
// not send a blocked .onion name.
TEST_F(OnionHostnameResolutionDnsClientTest, BlocksBootstrapFollowup) {
  MockDnsClientRuleList rules;
  rules.emplace_back(
      "doh.example.onion", dns_protocol::kTypeA,
      /*secure=*/true,
      MockDnsClientRule::Result(MockDnsClientRule::ResultType::kOk),
      /*delay=*/true);
  SetUpDnsClient(SecureDnsMode::kAutomatic, std::move(rules));
  const url::SchemeHostPort endpoint(url::kHttpsScheme, "doh.example.onion",
                                     443);
  dns_client_->set_preset_endpoint(endpoint);
  dns_client_->set_preset_addrs({IPEndPoint(IPAddress(192, 0, 2, 1), 443)});

  std::unique_ptr<HostResolver::ResolveHostRequest> request =
      CreateRequest(endpoint, SecureDnsPolicy::kBootstrap);
  TestCompletionCallback callback;
  EXPECT_EQ(OK, callback.GetResult(request->Start(callback.callback())));

  EXPECT_FALSE(
      dns_client_->CompleteOneDelayedTransactionOfType(DnsQueryType::A));
}

// Dropping the DoH and DNS tasks keeps the insecure cache lookup queued after
// them in automatic mode.
TEST_F(OnionHostnameResolutionDnsClientTest, ServesInsecureCacheWhenBlocked) {
  MockDnsClientRuleList rules;
  rules.emplace_back(
      "example.onion", dns_protocol::kTypeA, /*secure=*/true,
      MockDnsClientRule::Result(MockDnsClientRule::ResultType::kFail),
      /*delay=*/true);
  rules.emplace_back(
      "example.onion", dns_protocol::kTypeA, /*secure=*/false,
      MockDnsClientRule::Result(MockDnsClientRule::ResultType::kOk),
      /*delay=*/false);
  SetUpDnsClient(SecureDnsMode::kAutomatic, std::move(rules),
                 /*insecure_dns=*/true);

  // Cache an insecure answer while .onion is allowed.
  context_->set_tor_mode(URLRequestContext::TorMode::kPassthrough);
  std::unique_ptr<HostResolver::ResolveHostRequest> request =
      CreateRequest("example.onion");
  TestCompletionCallback callback;
  ASSERT_EQ(ERR_IO_PENDING, request->Start(callback.callback()));
  dns_client_->CompleteDelayedTransactions();
  ASSERT_EQ(OK, callback.WaitForResult());

  context_->set_tor_mode(URLRequestContext::TorMode::kDisabled);
  request = CreateRequest("example.onion");
  TestCompletionCallback blocked_callback;
  EXPECT_EQ(OK, blocked_callback.GetResult(
                    request->Start(blocked_callback.callback())));
  EXPECT_FALSE(
      dns_client_->CompleteOneDelayedTransactionOfType(DnsQueryType::A));
}

// When the system resolver would run next, the hosts file is skipped in
// favor of getaddrinfo(). A blocked lookup still gets the hosts file answer.
TEST_F(OnionHostnameResolutionDnsClientTest, ServesHostsFileWhenBlocked) {
  DnsHosts hosts;
  hosts[DnsHostsKey("example.onion", ADDRESS_FAMILY_IPV4)] =
      IPAddress(10, 0, 0, 1);
  SetUpDnsClient(SecureDnsMode::kOff, MockDnsClientRuleList(),
                 /*insecure_dns=*/false, std::move(hosts));

  std::unique_ptr<HostResolver::ResolveHostRequest> request =
      CreateRequest("example.onion");
  TestCompletionCallback callback;
  EXPECT_EQ(OK, callback.GetResult(request->Start(callback.callback())));
  ASSERT_EQ(1u, request->GetAddressResults().size());
  EXPECT_EQ(IPAddress(10, 0, 0, 1),
            request->GetAddressResults().front().address());
  EXPECT_EQ(0u, NumSystemResolves());
}

// Limits the resolver to two concurrent jobs, so a job can wait for a
// dispatcher slot.
class OnionHostnameResolutionDispatcherTest
    : public OnionHostnameResolutionDnsClientTest {
 public:
  OnionHostnameResolutionDispatcherTest() {
    manager_options_.max_concurrent_resolves = 2;
  }

  void SetUp() override {
    OnionHostnameResolutionDnsClientTest::SetUp();
    host_resolver_->GetManagerForTesting()->SetLastIPv6ProbeResultForTesting(
        true);
  }

  // Delays AAAA lookups of busy.test and A lookups of busy2.test. .onion
  // lookups that reach DNS are delayed when `delay_onion` is set and fail the
  // test otherwise.
  MockDnsClientRuleList Rules(bool delay_onion) {
    MockDnsClientRuleList rules;
    rules.emplace_back(
        "busy.test", dns_protocol::kTypeAAAA, /*secure=*/false,
        MockDnsClientRule::Result(MockDnsClientRule::ResultType::kOk),
        /*delay=*/true);
    rules.emplace_back(
        "busy2.test", dns_protocol::kTypeA, /*secure=*/false,
        MockDnsClientRule::Result(MockDnsClientRule::ResultType::kOk),
        /*delay=*/true);
    for (uint16_t qtype : {dns_protocol::kTypeA, dns_protocol::kTypeAAAA}) {
      rules.emplace_back(
          "example.onion", qtype, /*secure=*/false,
          MockDnsClientRule::Result(
              delay_onion ? MockDnsClientRule::ResultType::kOk
                          : MockDnsClientRule::ResultType::kUnexpected),
          /*delay=*/delay_onion);
    }
    return rules;
  }

  // Expects both dispatcher slots to be free: lookups of busy.test (AAAA) and
  // busy2.test (A) must both get a slot and start their DNS transaction.
  void ExpectDispatcherHasFreeSlots() {
    std::unique_ptr<HostResolver::ResolveHostRequest> first =
        CreateRequest(url::SchemeHostPort(url::kHttpsScheme, "busy.test", 9443),
                      SecureDnsPolicy::kAllow, DnsQueryType::AAAA);
    TestCompletionCallback first_callback;
    ASSERT_EQ(ERR_IO_PENDING, first->Start(first_callback.callback()));
    std::unique_ptr<HostResolver::ResolveHostRequest> second =
        CreateRequest("busy2.test");
    TestCompletionCallback second_callback;
    ASSERT_EQ(ERR_IO_PENDING, second->Start(second_callback.callback()));

    // Fails if `second` is still waiting for a slot.
    ASSERT_TRUE(
        dns_client_->CompleteOneDelayedTransactionOfType(DnsQueryType::A));
    ASSERT_TRUE(
        dns_client_->CompleteOneDelayedTransactionOfType(DnsQueryType::AAAA));
    EXPECT_EQ(OK, first_callback.WaitForResult());
    EXPECT_EQ(OK, second_callback.WaitForResult());
  }
};

// A job that starts blocking .onion while it waits for a dispatcher slot fails
// once it gets one, and gives the slot back.
TEST_F(OnionHostnameResolutionDispatcherTest, FailsQueuedJob) {
  SetUpDnsClient(SecureDnsMode::kOff, Rules(/*delay_onion=*/false),
                 /*insecure_dns=*/true);
  context_->set_tor_mode(URLRequestContext::TorMode::kPassthrough);

  // Fill both slots.
  std::unique_ptr<HostResolver::ResolveHostRequest> busy1 =
      CreateRequest("busy.test", DnsQueryType::AAAA);
  TestCompletionCallback busy1_callback;
  ASSERT_EQ(ERR_IO_PENDING, busy1->Start(busy1_callback.callback()));
  std::unique_ptr<HostResolver::ResolveHostRequest> busy2 =
      CreateRequest(url::SchemeHostPort(url::kHttpsScheme, "busy.test", 8443),
                    SecureDnsPolicy::kAllow, DnsQueryType::AAAA);
  TestCompletionCallback busy2_callback;
  ASSERT_EQ(ERR_IO_PENDING, busy2->Start(busy2_callback.callback()));

  std::unique_ptr<HostResolver::ResolveHostRequest> request =
      CreateRequest("example.onion");
  TestCompletionCallback callback;
  ASSERT_EQ(ERR_IO_PENDING, request->Start(callback.callback()));

  context_->set_tor_mode(URLRequestContext::TorMode::kDisabled);
  dns_client_->CompleteDelayedTransactions();
  EXPECT_EQ(OK, busy1_callback.WaitForResult());

  EXPECT_EQ(ERR_NAME_NOT_RESOLVED, callback.WaitForResult());
  EXPECT_EQ(0u, NumSystemResolves());
  ExpectDispatcherHasFreeSlots();
}

// A running DNS task whose next transaction waits for a dispatcher slot stops
// once blocking starts, instead of sending that transaction.
TEST_F(OnionHostnameResolutionDispatcherTest, StopsDnsTaskWaitingForSlot) {
  SetUpDnsClient(SecureDnsMode::kOff, Rules(/*delay_onion=*/true),
                 /*insecure_dns=*/true);
  context_->set_tor_mode(URLRequestContext::TorMode::kPassthrough);

  // Take one slot, so the .onion A and AAAA transactions cannot both run.
  std::unique_ptr<HostResolver::ResolveHostRequest> busy =
      CreateRequest("busy.test", DnsQueryType::AAAA);
  TestCompletionCallback busy_callback;
  ASSERT_EQ(ERR_IO_PENDING, busy->Start(busy_callback.callback()));

  std::unique_ptr<HostResolver::ResolveHostRequest> request =
      CreateRequest("example.onion", DnsQueryType::UNSPECIFIED);
  TestCompletionCallback callback;
  ASSERT_EQ(ERR_IO_PENDING, request->Start(callback.callback()));

  context_->set_tor_mode(URLRequestContext::TorMode::kDisabled);
  ASSERT_TRUE(
      dns_client_->CompleteOneDelayedTransactionOfType(DnsQueryType::AAAA));
  EXPECT_EQ(OK, busy_callback.WaitForResult());

  EXPECT_EQ(ERR_NAME_NOT_RESOLVED, callback.WaitForResult());
  EXPECT_EQ(0u, NumSystemResolves());
  // The transaction in flight was cancelled and the waiting one never started.
  EXPECT_FALSE(
      dns_client_->CompleteOneDelayedTransactionOfType(DnsQueryType::A));
  EXPECT_FALSE(
      dns_client_->CompleteOneDelayedTransactionOfType(DnsQueryType::AAAA));
  ExpectDispatcherHasFreeSlots();
}

#else  // BUILDFLAG(ENABLE_TOR)

// Builds without Tor never block .onion, even in a context left at the default
// kDisabled.
TEST_F(OnionHostnameResolutionTest, ResolvesOnionWithoutTor) {
  EXPECT_EQ(URLRequestContext::TorMode::kDisabled, context_->tor_mode());

  EXPECT_EQ(OK, Resolve("example.onion"));
  EXPECT_EQ(1u, NumSystemResolves());
}

#endif  // BUILDFLAG(ENABLE_TOR)

}  // namespace net
