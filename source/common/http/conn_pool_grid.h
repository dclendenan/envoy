#pragma once

#include "common/http/conn_pool_base.h"

#include "absl/container/flat_hash_map.h"

namespace Envoy {
namespace Http {

// An HTTP connection pool which will handle the connectivity grid of
// [WiFi / cellular] [ipv4 / ipv6] [QUIC / TCP].
// Currently only [QUIC / TCP are handled]
class ConnectivityGrid : public ConnectionPool::Instance {
public:
  struct ConnectivityOptions {
    explicit ConnectivityOptions(const std::vector<Http::Protocol>& protocols)
        : protocols_(protocols) {}
    std::vector<Http::Protocol> protocols_;
  };

  using PoolIterator = std::list<ConnectionPool::InstancePtr>::iterator;

  // This is a class which wraps a caller's connection pool callbacks to
  // auto-retry pools in the case of connection failure.
  //
  // It also relays cancellation calls between the original caller and the
  // pools currently trying to connect.
  //
  // TODO(#15649) currently this tries one connection at a time.
  // It should instead have a timer of its own and start the second connection
  // in parallel after a suitable delay.
  class WrapperCallbacks : public ConnectionPool::Callbacks,
                           public ConnectionPool::Cancellable,
                           public LinkedObject<WrapperCallbacks> {
  public:
    WrapperCallbacks(ConnectivityGrid& grid, Http::ResponseDecoder& decoder, PoolIterator pool_it,
                     ConnectionPool::Callbacks& callbacks);

    // ConnectionPool::Callbacks
    void onPoolFailure(ConnectionPool::PoolFailureReason reason,
                       absl::string_view transport_failure_reason,
                       Upstream::HostDescriptionConstSharedPtr host) override;
    void onPoolReady(RequestEncoder& encoder, Upstream::HostDescriptionConstSharedPtr host,
                     const StreamInfo::StreamInfo& info,
                     absl::optional<Http::Protocol> protocol) override;

    // ConnectionPool::Cancellable
    void cancel(Envoy::ConnectionPool::CancelPolicy cancel_policy) override;

    // Removes this from the owning list, deleting it.
    void deleteThis();

    ConnectionPool::Instance& pool() { return **pool_it_; }
    // The owning grid.
    ConnectivityGrid& grid_;
    // The decoder for the original newStream, needed to create streams on subsequent pools.
    Http::ResponseDecoder& decoder_;
    // The callbacks from the original caller, which must get onPoolFailure or
    // onPoolReady unless there is call to cancel().
    ConnectionPool::Callbacks& inner_callbacks_;
    // The current pool being connected to.
    PoolIterator pool_it_;
    // The handle to cancel the request to the current pool.
    // This is owned by the pool which created it.
    Cancellable* cancellable_;
  };
  using WrapperCallbacksPtr = std::unique_ptr<WrapperCallbacks>;

  ConnectivityGrid(Event::Dispatcher& dispatcher, Random::RandomGenerator& random_generator,
                   Upstream::HostConstSharedPtr host, Upstream::ResourcePriority priority,
                   const Network::ConnectionSocket::OptionsSharedPtr& options,
                   const Network::TransportSocketOptionsSharedPtr& transport_socket_options,
                   Upstream::ClusterConnectivityState& state, TimeSource& time_source,
                   ConnectivityOptions connectivity_options);
  ~ConnectivityGrid() override;

  // Http::ConnPool::Instance
  bool hasActiveConnections() const override;
  ConnectionPool::Cancellable* newStream(Http::ResponseDecoder& response_decoder,
                                         ConnectionPool::Callbacks& callbacks) override;
  void addDrainedCallback(DrainedCb cb) override;
  void drainConnections() override;
  Upstream::HostDescriptionConstSharedPtr host() const override;
  bool maybePreconnect(float preconnect_ratio) override;

  // Returns the next pool in the ordered priority list.
  absl::optional<PoolIterator> nextPool(PoolIterator pool_it);

private:
  friend class ConnectivityGridForTest;

  // Called by each pool as it drains. The grid is responsible for calling
  // drained_callbacks_ once all pools have drained.
  void onDrainReceived();

  // Creates the next pool in the priority list, or absl::nullopt if all pools
  // have been created.
  virtual absl::optional<PoolIterator> createNextPool();

  // This batch of member variables are latched objects required for pool creation.
  Event::Dispatcher& dispatcher_;
  Random::RandomGenerator& random_generator_;
  Upstream::HostConstSharedPtr host_;
  Upstream::ResourcePriority priority_;
  const Network::ConnectionSocket::OptionsSharedPtr& options_;
  const Network::TransportSocketOptionsSharedPtr& transport_socket_options_;
  Upstream::ClusterConnectivityState& state_;
  TimeSource& time_source_;

  // Tracks how many drains are needed before calling drain callbacks. This is
  // set to the number of pools when the first drain callbacks are added, and
  // decremented as various pools drain.
  uint32_t drains_needed_ = 0;
  // Tracks the callbacks to be called on drain completion.
  std::list<Instance::DrainedCb> drained_callbacks_;

  // The connection pools to use to create new streams, ordered in the order of
  // desired use.
  std::list<ConnectionPool::InstancePtr> pools_;
  // True iff under the stack of the destructor, to avoid calling drain
  // callbacks on deletion.
  bool destroying_{};

  // Wrapped callbacks are stashed in the wrapped_callbacks_ for ownership.
  std::list<WrapperCallbacksPtr> wrapped_callbacks_;
};

} // namespace Http
} // namespace Envoy
