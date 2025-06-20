#include "source/extensions/filters/network/redis_proxy/config.h"

#include "envoy/config/core/v3/grpc_service.pb.h"
#include "envoy/extensions/filters/network/redis_proxy/v3/redis_proxy.pb.h"
#include "envoy/extensions/filters/network/redis_proxy/v3/redis_proxy.pb.validate.h"

#include "source/extensions/common/dynamic_forward_proxy/dns_cache_manager_impl.h"
#include "source/extensions/common/redis/cluster_refresh_manager_impl.h"
#include "source/extensions/filters/network/common/redis/aws_iam_authenticator_impl.h"
#include "source/extensions/filters/network/common/redis/client_impl.h"
#include "source/extensions/filters/network/common/redis/fault_impl.h"
#include "source/extensions/filters/network/redis_proxy/command_splitter_impl.h"
#include "source/extensions/filters/network/redis_proxy/proxy_filter.h"
#include "source/extensions/filters/network/redis_proxy/router_impl.h"

#include "absl/container/flat_hash_set.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace RedisProxy {

namespace {
inline void addUniqueClusters(
    absl::flat_hash_set<std::string>& clusters,
    const envoy::extensions::filters::network::redis_proxy::v3::RedisProxy::PrefixRoutes::Route&
        route) {
  clusters.emplace(route.cluster());
  for (auto& mirror : route.request_mirror_policy()) {
    clusters.emplace(mirror.cluster());
  }
  if (route.has_read_command_policy()) {
    clusters.emplace(route.read_command_policy().cluster());
  }
}
} // namespace

Network::FilterFactoryCb RedisProxyFilterConfigFactory::createFilterFactoryFromProtoTyped(
    const envoy::extensions::filters::network::redis_proxy::v3::RedisProxy& proto_config,
    Server::Configuration::FactoryContext& context) {

  auto& server_context = context.serverFactoryContext();

  ASSERT(!proto_config.stat_prefix().empty());
  ASSERT(proto_config.has_settings());

  Extensions::Common::Redis::ClusterRefreshManagerSharedPtr refresh_manager =
      Extensions::Common::Redis::getClusterRefreshManager(
          server_context.singletonManager(), server_context.mainThreadDispatcher(),
          server_context.clusterManager(), server_context.timeSource());

  Extensions::Common::DynamicForwardProxy::DnsCacheManagerFactoryImpl cache_manager_factory(
      context);

  auto filter_config = std::make_shared<ProxyFilterConfig>(
      proto_config, context.scope(), context.drainDecision(), server_context.runtime(),
      server_context.api(), context.serverFactoryContext().timeSource(), cache_manager_factory);

  envoy::extensions::filters::network::redis_proxy::v3::RedisProxy::PrefixRoutes prefix_routes(
      proto_config.prefix_routes());

  // Set the catch-all route from the settings parameters.
  if (prefix_routes.routes_size() == 0 && !prefix_routes.has_catch_all_route()) {
    throw EnvoyException("cannot configure a redis-proxy without any upstream");
  }

  absl::flat_hash_set<std::string> unique_clusters;
  for (auto& route : prefix_routes.routes()) {
    addUniqueClusters(unique_clusters, route);
  }
  addUniqueClusters(unique_clusters, prefix_routes.catch_all_route());

  auto redis_command_stats =
      Common::Redis::RedisCommandStats::createRedisCommandStats(context.scope().symbolTable());

  Upstreams upstreams;
  for (auto& cluster : unique_clusters) {

    // Create the AWS IAM authenticator if required
    absl::optional<Common::Redis::AwsIamAuthenticator::AwsIamAuthenticatorSharedPtr>
        aws_iam_authenticator;
    absl::optional<envoy::extensions::filters::network::redis_proxy::v3::AwsIam> aws_iam_config;
    auto cluster_optref = server_context.clusterManager().clusters().getCluster(cluster);
    if (cluster_optref.has_value()) {
      // Does our cluster have an AwsIam element available? If so, create a new authenticator for
      // this connection pool.
      aws_iam_config = ProtocolOptionsConfigImpl::awsIamConfig(cluster_optref.value().get().info());
      if (aws_iam_config.has_value()) {
        if (!ProtocolOptionsConfigImpl::authUsername(cluster_optref.value().get().info(),
                                                     context.serverFactoryContext().api())
                 .empty()) {
          aws_iam_authenticator = Common::Redis::AwsIamAuthenticator::AwsIamAuthenticatorFactory::
              initAwsIamAuthenticator(server_context, aws_iam_config.value());
        } else {
          ENVOY_LOG_MISC(warn,
                         "No auth_username found for cluster {}, AWS IAM Authentication will be "
                         "disabled for this cluster",
                         cluster);
        }
      }
    }

    Stats::ScopeSharedPtr stats_scope =
        context.scope().createScope(fmt::format("cluster.{}.redis_cluster", cluster));
    auto conn_pool_ptr = std::make_shared<ConnPool::InstanceImpl>(
        cluster, server_context.clusterManager(),
        Common::Redis::Client::ClientFactoryImpl::instance_, server_context.threadLocal(),
        proto_config.settings(), server_context.api(), std::move(stats_scope), redis_command_stats,
        refresh_manager, filter_config->dns_cache_, aws_iam_config, aws_iam_authenticator);
    conn_pool_ptr->init();
    upstreams.emplace(cluster, conn_pool_ptr);
  }

  auto router =
      std::make_unique<PrefixRoutes>(prefix_routes, std::move(upstreams), server_context.runtime());

  auto fault_manager = std::make_unique<Common::Redis::FaultManagerImpl>(
      server_context.api().randomGenerator(), server_context.runtime(), proto_config.faults());

  absl::flat_hash_set<std::string> custom_commands;
  for (const auto& cmd : proto_config.custom_commands()) {
    custom_commands.insert(cmd);
  }

  std::shared_ptr<CommandSplitter::Instance> splitter =
      std::make_shared<CommandSplitter::InstanceImpl>(
          std::move(router), context.scope(), filter_config->stat_prefix_,
          server_context.timeSource(), proto_config.latency_in_micros(), std::move(fault_manager),
          std::move(custom_commands));

  auto has_external_auth_provider_ = proto_config.has_external_auth_provider();
  auto grpc_service = proto_config.external_auth_provider().grpc_service();
  auto timeout_ms = PROTOBUF_GET_MS_OR_DEFAULT(grpc_service, timeout, 200);

  return [has_external_auth_provider_, grpc_service, &context, splitter, filter_config,
          timeout_ms](Network::FilterManager& filter_manager) -> void {
    Common::Redis::DecoderFactoryImpl decoder_factory;

    ExternalAuth::ExternalAuthClientPtr&& auth_client{nullptr};
    if (has_external_auth_provider_) {
      auto auth_client_factory_or_error =
          context.serverFactoryContext()
              .clusterManager()
              .grpcAsyncClientManager()
              .factoryForGrpcService(grpc_service, context.scope(), true);
      THROW_IF_NOT_OK_REF(auth_client_factory_or_error.status());

      auth_client = std::make_unique<ExternalAuth::GrpcExternalAuthClient>(
          THROW_OR_RETURN_VALUE(
              auth_client_factory_or_error.value()->createUncachedRawAsyncClient(),
              Grpc::RawAsyncClientPtr),
          std::chrono::milliseconds(timeout_ms));
    }

    filter_manager.addReadFilter(std::make_shared<ProxyFilter>(
        decoder_factory, Common::Redis::EncoderPtr{new Common::Redis::EncoderImpl()}, *splitter,
        filter_config, std::move(auth_client)));
  };
}

/**
 * Static registration for the redis filter. @see RegisterFactory.
 */
LEGACY_REGISTER_FACTORY(RedisProxyFilterConfigFactory,
                        Server::Configuration::NamedNetworkFilterConfigFactory,
                        "envoy.redis_proxy");

} // namespace RedisProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
