#include "common/http/conn_pool_grid.h"

#include "test/common/http/common.h"
#include "test/common/upstream/utility.h"
#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/http/conn_pool.h"
#include "test/mocks/http/stream_decoder.h"
#include "test/mocks/http/stream_encoder.h"
#include "test/mocks/network/connection.h"
#include "test/mocks/ssl/mocks.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;

namespace Envoy {
namespace Http {

class ConnectivityGridForTest : public ConnectivityGrid {
public:
  using ConnectivityGrid::ConnectivityGrid;

  static absl::optional<PoolIterator> forceCreateNextPool(ConnectivityGrid& grid) {
    return grid.createNextPool();
  }

  absl::optional<ConnectivityGrid::PoolIterator> createNextPool() override {
    if (pools_.size() == 2) {
      return absl::nullopt;
    }
    ConnectionPool::MockInstance* instance = new NiceMock<ConnectionPool::MockInstance>();
    pools_.push_back(ConnectionPool::InstancePtr{instance});
    ON_CALL(*instance, newStream(_, _))
        .WillByDefault(
            Invoke([&](Http::ResponseDecoder&,
                       ConnectionPool::Callbacks& callbacks) -> ConnectionPool::Cancellable* {
              callbacks_.push_back(&callbacks);
              return &cancel_;
            }));
    if (pools_.size() == 1) {
      return pools_.begin();
    }
    return ++pools_.begin();
  }

  ConnectionPool::MockInstance* first() {
    if (pools_.empty()) {
      return nullptr;
    }
    return static_cast<ConnectionPool::MockInstance*>(&*pools_.front());
  }
  ConnectionPool::MockInstance* second() {
    if (pools_.size() < 2) {
      return nullptr;
    }
    return static_cast<ConnectionPool::MockInstance*>(&**(++pools_.begin()));
  }

  ConnectionPool::Callbacks* callbacks(int index = 0) { return callbacks_[index]; }

  void setDestroying() { destroying_ = true; }
  std::vector<ConnectionPool::Callbacks*> callbacks_;
  NiceMock<Envoy::ConnectionPool::MockCancellable> cancel_;
};

namespace {
class ConnectivityGridTest : public Event::TestUsingSimulatedTime, public testing::Test {
public:
  ConnectivityGridTest()
      : options_({Http::Protocol::Http11, Http::Protocol::Http2, Http::Protocol::Http3}),
        host_(new NiceMock<Upstream::MockHostDescription>()),
        grid_(dispatcher_, random_,
              Upstream::makeTestHost(cluster_, "tcp://127.0.0.1:9000", simTime()),
              Upstream::ResourcePriority::Default, socket_options_, transport_socket_options_,
              state_, simTime(), options_) {}

  const Network::ConnectionSocket::OptionsSharedPtr socket_options_;
  const Network::TransportSocketOptionsSharedPtr transport_socket_options_;
  ConnectivityGrid::ConnectivityOptions options_;
  Upstream::HostDescriptionConstSharedPtr host_;
  Upstream::ClusterConnectivityState state_;
  NiceMock<Event::MockDispatcher> dispatcher_;
  std::shared_ptr<Upstream::MockClusterInfo> cluster_{new NiceMock<Upstream::MockClusterInfo>()};
  NiceMock<Random::MockRandomGenerator> random_;
  ConnectivityGridForTest grid_;

  NiceMock<ConnPoolCallbacks> callbacks_;
  NiceMock<MockResponseDecoder> decoder_;

  StreamInfo::MockStreamInfo info_;
  NiceMock<MockRequestEncoder> encoder_;
};

// Test the first pool successfully connecting.
TEST_F(ConnectivityGridTest, Success) {
  EXPECT_EQ(grid_.first(), nullptr);

  grid_.newStream(decoder_, callbacks_);
  EXPECT_NE(grid_.first(), nullptr);

  // onPoolReady should be passed from the pool back to the original caller.
  ASSERT_NE(grid_.callbacks(), nullptr);
  EXPECT_CALL(callbacks_.pool_ready_, ready());
  grid_.callbacks()->onPoolReady(encoder_, host_, info_, absl::nullopt);
}

// Test the first pool failing and the second connecting.
TEST_F(ConnectivityGridTest, FailureThenSuccess) {
  EXPECT_EQ(grid_.first(), nullptr);

  grid_.newStream(decoder_, callbacks_);
  EXPECT_NE(grid_.first(), nullptr);

  // onPoolFailure should not be passed up the first time. Instead the grid
  // should fail over to the second pool.
  EXPECT_CALL(callbacks_.pool_failure_, ready()).Times(0);
  grid_.callbacks()->onPoolFailure(ConnectionPool::PoolFailureReason::LocalConnectionFailure,
                                   "reason", host_);
  ASSERT_NE(grid_.second(), nullptr);

  // onPoolReady should be passed from the pool back to the original caller.
  ASSERT_NE(grid_.callbacks(), nullptr);
  EXPECT_CALL(callbacks_.pool_ready_, ready());
  grid_.callbacks()->onPoolReady(encoder_, host_, info_, absl::nullopt);
}

// Test that after the first pool fails, subsequent connections will
// successfully fail over to the second pool (the iterators work as intended)
TEST_F(ConnectivityGridTest, FailureThenSuccessForMultipleConnections) {
  NiceMock<ConnPoolCallbacks> callbacks2;
  NiceMock<MockResponseDecoder> decoder2;
  // Kick off two new streams.
  auto* cancel1 = grid_.newStream(decoder_, callbacks_);
  auto* cancel2 = grid_.newStream(decoder2, callbacks2);

  // Fail the first connection and verify the second pool is created.
  EXPECT_CALL(callbacks_.pool_failure_, ready()).Times(0);
  grid_.callbacks()->onPoolFailure(ConnectionPool::PoolFailureReason::LocalConnectionFailure,
                                   "reason", host_);
  ASSERT_NE(grid_.second(), nullptr);

  // Fail the second connection, and verify the second pool gets another newStream call.
  EXPECT_CALL(callbacks_.pool_failure_, ready()).Times(0);
  EXPECT_CALL(*grid_.second(), newStream(_, _));
  grid_.callbacks(1)->onPoolFailure(ConnectionPool::PoolFailureReason::LocalConnectionFailure,
                                    "reason", host_);

  // Clean up.
  cancel1->cancel(Envoy::ConnectionPool::CancelPolicy::CloseExcess);
  cancel2->cancel(Envoy::ConnectionPool::CancelPolicy::CloseExcess);
}

// Test both pools failing
TEST_F(ConnectivityGridTest, FailureTwice) {
  EXPECT_EQ(grid_.first(), nullptr);

  auto cancel = grid_.newStream(decoder_, callbacks_);
  EXPECT_NE(grid_.first(), nullptr);

  // cancel should be passed through the WrapperCallbacks to the connection pool.
  EXPECT_CALL(grid_.cancel_, cancel(_));
  cancel->cancel(Envoy::ConnectionPool::CancelPolicy::CloseExcess);
}

// Make sure drains get sent to all active pools.
TEST_F(ConnectivityGridTest, Drain) {
  grid_.drainConnections();

  // Synthetically create a pool.
  grid_.createNextPool();
  {
    EXPECT_CALL(*grid_.first(), drainConnections());
    grid_.drainConnections();
  }

  grid_.createNextPool();
  {
    EXPECT_CALL(*grid_.first(), drainConnections());
    EXPECT_CALL(*grid_.second(), drainConnections());
    grid_.drainConnections();
  }
}

// Make sure drain callbacks work as expected.
TEST_F(ConnectivityGridTest, DrainCallbacks) {
  // Synthetically create both pools.
  grid_.createNextPool();
  grid_.createNextPool();

  bool drain_received = false;
  bool second_drain_received = false;

  ConnectionPool::Instance::DrainedCb pool1_cb;
  ConnectionPool::Instance::DrainedCb pool2_cb;
  // The first time a drained callback is added, the Grid's callback should be
  // added to both pools.
  {
    EXPECT_CALL(*grid_.first(), addDrainedCallback(_))
        .WillOnce(Invoke(Invoke([&](ConnectionPool::Instance::DrainedCb cb) { pool1_cb = cb; })));
    EXPECT_CALL(*grid_.second(), addDrainedCallback(_))
        .WillOnce(Invoke(Invoke([&](ConnectionPool::Instance::DrainedCb cb) { pool2_cb = cb; })));
    grid_.addDrainedCallback([&drain_received]() -> void { drain_received = true; });
  }

  // The second time a drained callback is added, the pools will not see any
  // change.
  {
    EXPECT_CALL(*grid_.first(), addDrainedCallback(_)).Times(0);
    EXPECT_CALL(*grid_.second(), addDrainedCallback(_)).Times(0);
    grid_.addDrainedCallback([&second_drain_received]() -> void { second_drain_received = true; });
  }
  {
    // Notify the grid the second pool has been drained. This should not be
    // passed up to the original callers.
    EXPECT_FALSE(drain_received);
    (pool2_cb)();
    EXPECT_FALSE(drain_received);
  }

  {
    // Notify the grid that another pool has been drained. Now that all pools are
    // drained, the original callers should be informed.
    EXPECT_FALSE(drain_received);
    (pool1_cb)();
    EXPECT_TRUE(drain_received);
    EXPECT_TRUE(second_drain_received);
  }
}

// Ensure drain callbacks aren't called during grid teardown.
TEST_F(ConnectivityGridTest, NoDrainOnTeardown) {
  grid_.createNextPool();

  bool drain_received = false;
  ConnectionPool::Instance::DrainedCb pool1_cb;

  {
    EXPECT_CALL(*grid_.first(), addDrainedCallback(_))
        .WillOnce(Invoke(Invoke([&](ConnectionPool::Instance::DrainedCb cb) { pool1_cb = cb; })));
    grid_.addDrainedCallback([&drain_received]() -> void { drain_received = true; });
  }

  grid_.setDestroying(); // Fake being in the destructor.
  (pool1_cb)();
  EXPECT_FALSE(drain_received);
}

#ifdef ENVOY_ENABLE_QUICHE

} // namespace
} // namespace Http
} // namespace Envoy
#include "extensions/quic_listeners/quiche/quic_transport_socket_factory.h"
namespace Envoy {
namespace Http {
namespace {

TEST_F(ConnectivityGridTest, RealGrid) {
  testing::InSequence s;
  dispatcher_.allow_null_callback_ = true;
  // Set the cluster up to have a quic transport socket.
  Envoy::Ssl::ClientContextConfigPtr config(new NiceMock<Ssl::MockClientContextConfig>());
  auto factory = std::make_unique<Quic::QuicClientTransportSocketFactory>(std::move(config));
  auto& matcher =
      static_cast<Upstream::MockTransportSocketMatcher&>(*cluster_->transport_socket_matcher_);
  EXPECT_CALL(matcher, resolve(_))
      .WillRepeatedly(testing::Return(
          Upstream::TransportSocketMatcher::MatchData(*factory, matcher.stats_, "test")));

  ConnectivityGrid grid(dispatcher_, random_,
                        Upstream::makeTestHost(cluster_, "tcp://127.0.0.1:9000", simTime()),
                        Upstream::ResourcePriority::Default, socket_options_,
                        transport_socket_options_, state_, simTime(), options_);

  // Create the HTTP/3 pool.
  auto optional_it1 = ConnectivityGridForTest::forceCreateNextPool(grid);
  ASSERT_TRUE(optional_it1.has_value());

  // Create the mixed pool.
  auto optional_it2 = ConnectivityGridForTest::forceCreateNextPool(grid);
  ASSERT_TRUE(optional_it2.has_value());
  // There is no third option currently.
  auto optional_it3 = ConnectivityGridForTest::forceCreateNextPool(grid);
  ASSERT_FALSE(optional_it3.has_value());
}
#endif

} // namespace
} // namespace Http
} // namespace Envoy
