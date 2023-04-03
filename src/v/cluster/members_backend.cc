#include "cluster/members_backend.h"

#include "cluster/controller_api.h"
#include "cluster/errc.h"
#include "cluster/fwd.h"
#include "cluster/logger.h"
#include "cluster/members_frontend.h"
#include "cluster/members_manager.h"
#include "cluster/members_table.h"
#include "cluster/scheduling/allocation_strategy.h"
#include "cluster/topic_table.h"
#include "cluster/topics_frontend.h"
#include "cluster/types.h"
#include "config/configuration.h"
#include "model/metadata.h"
#include "model/namespace.h"
#include "model/timeout_clock.h"
#include "prometheus/prometheus_sanitize.h"
#include "random/generators.h"

#include <seastar/core/coroutine.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/sstring.hh>
#include <seastar/util/later.hh>
#include <seastar/util/optimized_optional.hh>

#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <fmt/ostream.h>

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <ostream>
#include <system_error>
#include <vector>

namespace cluster {
namespace {
struct node_replicas {
    size_t allocated_replicas;
    size_t max_capacity;
};

using node_replicas_map_t = absl::node_hash_map<model::node_id, node_replicas>;
static absl::node_hash_map<model::node_id, node_replicas>
calculate_replicas_per_node(
  const partition_allocator& allocator, partition_allocation_domain domain) {
    node_replicas_map_t ret;
    ret.reserve(allocator.state().allocation_nodes().size());

    for (const auto& [id, n] : allocator.state().allocation_nodes()) {
        if (!n->is_active()) {
            continue;
        }
        auto [it, _] = ret.try_emplace(
          id,
          node_replicas{
            .allocated_replicas = 0,
            .max_capacity = n->domain_partition_capacity(domain),
          });

        const auto domain_allocated = n->domain_allocated_partitions(domain);
        it->second.allocated_replicas += domain_allocated;
    }
    return ret;
}

static size_t
calculate_total_replicas(const node_replicas_map_t& node_replicas) {
    size_t total_replicas = 0;
    for (auto& [_, replicas] : node_replicas) {
        total_replicas += replicas.allocated_replicas;
    }
    return total_replicas;
}

void reassign_replicas(
  const model::ntp& ntp,
  partition_allocator& allocator,
  partition_assignment current_assignment,
  members_backend::partition_reallocation& reallocation) {
    // remove nodes that are going to be reassigned from current assignment.
    std::erase_if(
      current_assignment.replicas,
      [&reallocation](const model::broker_shard& bs) {
          return reallocation.replicas_to_remove.contains(bs.node_id);
      });

    auto res = allocator.reallocate_partition(
      reallocation.constraints.value(),
      current_assignment,
      get_allocation_domain(reallocation.ntp));
    if (res.has_value()) {
        reallocation.set_new_replicas(std::move(res.value()));
    } else {
        vlog(
          clusterlog.info,
          "failed to reallocate partition {} with assignment {}, error: {}",
          ntp,
          current_assignment.replicas,
          res.error());
    }
}

/**
 * The new replicas placement optimization stop condition is based on evenness
 * error.
 *
 * When there is no improvement of unevenness error after requesting partition
 * rebalancing the rebalancing stops.
 *
 * Error is calculated as:
 *
 *                                    N
 *                                   ___
 *                                   ╲    |R - r |
 *                                   ╱    |     n|
 *                                   ‾‾‾
 *                                  n = 0
 *                           e    = ──────────────
 *                            raw        T ⋅ N
 *
 *
 *                               T
 *                           R = ─
 *                               N
 *
 * Where:
 *
 * T - total number or replicas in cluster
 * R - requested replicas per node
 * N - number of nodes in the cluster
 * r_n - number of replicas on the node n
 *
 *
 * then the error is normalized to be in range [0,1].
 * To do this we calculate the maximum error:
 *                                              N
 *                                             ___
 *                                             ╲
 *                                  (T - R) +  ╱    R
 *                                             ‾‾‾
 *                                            n = 1
 *                           e    = ─────────────────
 *                            max         T ⋅ N
 *
 * normalized error is equal to:
 *
 *                                 e
 *                           e = ────
 *                               e
 *                                max
 *
 **/
double calculate_unevenness_error(
  const partition_allocator& allocator,
  const members_backend::update_meta& update,
  partition_allocation_domain domain) {
    static const std::vector<partition_allocation_domain> domains{
      partition_allocation_domains::consumer_offsets,
      partition_allocation_domains::common};

    const auto node_cnt = allocator.state().available_nodes();

    auto node_replicas = calculate_replicas_per_node(allocator, domain);
    /**
     * adjust per node replicas with the replicas that are going to be removed
     * from the node after successful reallocation
     */
    for (const auto& r : update.partition_reallocations) {
        if (r.allocation_units) {
            for (const auto& to_remove : r.replicas_to_remove) {
                auto it = node_replicas.find(to_remove);
                if (it != node_replicas.end()) {
                    it->second.allocated_replicas--;
                }
            }
        }
    }
    const auto total_replicas = calculate_total_replicas(node_replicas);

    if (total_replicas == 0) {
        return 0.0;
    }

    const auto target_replicas_per_node = total_replicas
                                          / allocator.state().available_nodes();
    // max error is an error calculated when all replicas are allocated on
    // the same node
    double max_err = (total_replicas - target_replicas_per_node)
                     + target_replicas_per_node * (node_cnt - 1);
    // divide by total replicas and node count to make the error independent
    // from number of nodes and number of replicas.
    max_err /= static_cast<double>(total_replicas);
    max_err /= static_cast<double>(node_cnt);

    double err = 0;
    for (auto& [id, allocation_info] : node_replicas) {
        double diff = static_cast<double>(target_replicas_per_node)
                      - static_cast<double>(allocation_info.allocated_replicas);

        vlog(
          clusterlog.trace,
          "node {} has {} replicas allocated in domain {}, requested replicas "
          "per node {}, difference: {}",
          id,
          allocation_info.allocated_replicas,
          domain,
          target_replicas_per_node,
          diff);
        err += std::abs(diff);
    }
    err /= (static_cast<double>(total_replicas) * node_cnt);

    // normalize error to stay in range (0,1)
    return err / max_err;
}

} // namespace

members_backend::members_backend(
  ss::sharded<cluster::topics_frontend>& topics_frontend,
  ss::sharded<topic_table>& topics,
  ss::sharded<partition_allocator>& allocator,
  ss::sharded<members_table>& members,
  ss::sharded<controller_api>& api,
  ss::sharded<members_manager>& members_manager,
  ss::sharded<members_frontend>& members_frontend,
  consensus_ptr raft0,
  ss::sharded<ss::abort_source>& as)
  : _topics_frontend(topics_frontend)
  , _topics(topics)
  , _allocator(allocator)
  , _members(members)
  , _api(api)
  , _members_manager(members_manager)
  , _members_frontend(members_frontend)
  , _reallocation_strategy(std::make_unique<default_reallocation_strategy>())
  , _raft0(raft0)
  , _as(as)
  , _retry_timeout(config::shard_local_cfg().members_backend_retry_ms())
  , _max_concurrent_reallocations(
      config::shard_local_cfg()
        .partition_autobalancing_concurrent_moves.bind()) {
    setup_metrics();
    ssx::spawn_with_gate(_bg, [this] {
        return ss::do_until(
          [this] { return _as.local().abort_requested(); },
          [this] { return handle_updates(); });
    });
}

ss::future<> members_backend::stop() {
    vlog(clusterlog.info, "Stopping Members Backend...");
    _new_updates.broken();
    return _bg.close();
}

void members_backend::setup_metrics() {
    if (config::shard_local_cfg().disable_metrics()) {
        return;
    }
    namespace sm = ss::metrics;
    _metrics.add_group(
      prometheus_sanitize::metrics_name("cluster:members:backend"),
      {
        sm::make_gauge(
          "queued_node_operations",
          [this] { return _updates.size(); },
          sm::description("Number of queued node operations")),
      });
}

void members_backend::start() { start_reconciliation_loop(); }

ss::future<> members_backend::handle_updates() {
    /**
     * wait for updates from members manager, after update is received we
     * translate it into reallocation meta, reallocation meta represents a
     * partition that needs reallocation
     */
    using updates_t = std::vector<members_manager::node_update>;
    return _members_manager.local()
      .get_node_updates()
      .then([this](updates_t updates) {
          return _lock.with([this, updates = std::move(updates)]() mutable {
              return ss::do_with(
                std::move(updates), [this](updates_t& updates) {
                    return ss::do_for_each(
                      updates, [this](members_manager::node_update update) {
                          return handle_single_update(update);
                      });
                });
          });
      })
      .handle_exception([](const std::exception_ptr& e) {
          vlog(clusterlog.trace, "error waiting for members updates - {}", e);
      });
}

ss::future<std::error_code> members_backend::request_rebalance() {
    if (!_raft0->is_leader()) {
        co_return errc::not_leader;
    }
    vlog(clusterlog.debug, "requesting on demand rebalance");
    auto u = co_await _lock.get_units();
    if (!_updates.empty()) {
        vlog(
          clusterlog.info,
          "can not trigger on demand rebalance, another update in progress");
        co_return errc::update_in_progress;
    }
    _updates.emplace_back();
    _new_updates.signal();
    co_return errc::success;
}

void members_backend::handle_single_update(
  members_manager::node_update update) {
    using update_t = members_manager::node_update_type;
    vlog(clusterlog.debug, "membership update received: {}", update);
    switch (update.type) {
    case update_t::recommissioned: {
        auto rev = handle_recommissioned_and_get_decomm_revision(update);
        _updates.emplace_back(update);
        _updates.back().decommission_update_revision = rev;
        _new_updates.signal();
        return;
    }
    case update_t::reallocation_finished:
        handle_reallocation_finished(update.id);
        return;
    case update_t::added:
        stop_node_decommissioning(update.id);
        _updates.emplace_back(update);
        _new_updates.signal();
        return;
    case update_t::decommissioned:
        _updates.emplace_back(update);
        stop_node_addition_and_ondemand_rebalance(update.id);
        _new_updates.signal();
        return;
    }
    __builtin_unreachable();
}

void members_backend::start_reconciliation_loop() {
    ssx::spawn_with_gate(_bg, [this] { return reconciliation_loop(); });
}

ss::future<> members_backend::reconciliation_loop() {
    while (!_as.local().abort_requested()) {
        try {
            auto ec = co_await reconcile();
            vlog(
              clusterlog.trace, "reconciliation loop result: {}", ec.message());
            if (!ec) {
                continue;
            }
        } catch (...) {
            vlog(
              clusterlog.info,
              "error encountered while handling cluster state reconciliation - "
              "{}",
              std::current_exception());
        }
        // when an error occurred wait before next retry
        co_await ss::sleep_abortable(_retry_timeout, _as.local());
    }
}

bool is_in_replica_set(
  const std::vector<model::broker_shard>& replicas, model::node_id id) {
    return std::any_of(
      replicas.cbegin(), replicas.cend(), [id](const model::broker_shard& bs) {
          return id == bs.node_id;
      });
}

ss::future<> members_backend::calculate_reallocations(update_meta& meta) {
    // on demand update
    if (!meta.update) {
        reallocations_for_even_partition_count(
          meta, partition_allocation_domains::consumer_offsets);
        if (
          meta.partition_reallocations.size()
          < _max_concurrent_reallocations()) {
            reallocations_for_even_partition_count(
              meta, partition_allocation_domains::common);
        }
        co_return;
    }
    // update caused by node event i.e. addition/decommissioning/recommissioning
    switch (meta.update->type) {
    case members_manager::node_update_type::decommissioned:
        co_await calculate_reallocations_after_decommissioned(meta);
        co_return;
    case members_manager::node_update_type::added:
        if (
          config::shard_local_cfg().partition_autobalancing_mode()
          == model::partition_autobalancing_mode::off) {
            co_return;
        }
        reallocations_for_even_partition_count(
          meta, partition_allocation_domains::consumer_offsets);
        if (
          meta.partition_reallocations.size()
          < _max_concurrent_reallocations()) {
            reallocations_for_even_partition_count(
              meta, partition_allocation_domains::common);
        }
        co_return;
    case members_manager::node_update_type::recommissioned:
        co_await calculate_reallocations_after_recommissioned(meta);
        co_return;
    case members_manager::node_update_type::reallocation_finished:
        co_return;
    }
}

void members_backend::reallocations_for_even_partition_count(
  members_backend::update_meta& meta, partition_allocation_domain domain) {
    _reallocation_strategy->reallocations_for_even_partition_count(
      _max_concurrent_reallocations(),
      _allocator.local(),
      _topics.local(),
      meta,
      domain);
}

void members_backend::default_reallocation_strategy::
  reallocations_for_even_partition_count(
    size_t max_batch_size,
    partition_allocator& allocator,
    topic_table& topics,
    members_backend::update_meta& meta,
    partition_allocation_domain domain) {
    size_t prev_reallocations_count = meta.partition_reallocations.size();
    calculate_reallocations_batch(
      max_batch_size, allocator, topics, meta, domain);
    auto current_error = calculate_unevenness_error(allocator, meta, domain);
    auto [it, _] = meta.last_unevenness_error.try_emplace(domain, 1.0);

    auto improvement = it->second - current_error;
    vlog(
      clusterlog.info,
      "[update: {}] unevenness error: {}, previous error: {}, improvement: {}",
      meta.update,
      current_error,
      it->second,
      improvement);

    it->second = current_error;

    // drop all reallocations if there is no improvement
    if (improvement <= 0) {
        meta.partition_reallocations.erase(
          std::next(
            meta.partition_reallocations.begin(),
            static_cast<long>(prev_reallocations_count)),
          meta.partition_reallocations.end());
    }
}

/**
 * Simple helper class representing how many replicas have to be moved from the
 * node.
 */
struct replicas_to_move {
    replicas_to_move(model::node_id id, uint32_t left)
      : id(id)
      , left_to_move(left) {}

    model::node_id id;
    uint32_t left_to_move;
    friend std::ostream&
    operator<<(std::ostream& o, const replicas_to_move& r) {
        fmt::print(o, "{{id: {}, left_to_move: {}}}", r.id, r.left_to_move);
        return o;
    }
};

ss::future<> members_backend::calculate_reallocations_after_decommissioned(
  members_backend::update_meta& meta) {
    vassert(
      meta.update,
      "decommissioning rebalance must be related with node update");
    // reallocate all partitions for which any of replicas is placed on
    // decommissioned node
    for (const auto& [tp_ns, cfg] : _topics.local().topics_map()) {
        if (!cfg.is_topic_replicable()) {
            continue;
        }

        for (const auto& pas : cfg.get_assignments()) {
            // break when we already scheduled more than allowed reallocations
            if (
              meta.partition_reallocations.size()
              >= _max_concurrent_reallocations()) {
                vlog(
                  clusterlog.info,
                  "reached limit of max concurrent reallocations: {}",
                  meta.partition_reallocations.size());
                break;
            }
            model::ntp ntp(tp_ns.ns, tp_ns.tp, pas.id);
            if (is_in_replica_set(pas.replicas, meta.update->id)) {
                auto previous_replica_set
                  = _topics.local().get_previous_replica_set(ntp);
                // update in progress, request cancel
                if (previous_replica_set) {
                    partition_reallocation reallocation(std::move(ntp));
                    reallocation.state = reallocation_state::request_cancel;
                    reallocation.current_replica_set = pas.replicas;
                    reallocation.new_replica_set = previous_replica_set.value();
                    meta.partition_reallocations.push_back(
                      std::move(reallocation));
                } else {
                    partition_reallocation reallocation(
                      std::move(ntp), pas.replicas.size());
                    reallocation.replicas_to_remove.emplace(meta.update->id);
                    meta.partition_reallocations.push_back(
                      std::move(reallocation));
                }
            }
        }
    }
    co_return;
}

bool is_reassigned_to_node(
  const members_backend::partition_reallocation& reallocation,
  model::node_id node_id) {
    if (!reallocation.allocation_units.has_value()) {
        return false;
    }
    return is_in_replica_set(
      reallocation.allocation_units->get_assignments().front().replicas,
      node_id);
}

void members_backend::default_reallocation_strategy::
  calculate_reallocations_batch(
    size_t max_batch_size,
    partition_allocator& allocator,
    topic_table& topics,
    members_backend::update_meta& meta,
    partition_allocation_domain domain) {
    // 1. count current node allocations
    auto node_replicas = calculate_replicas_per_node(allocator, domain);
    auto total_replicas = calculate_total_replicas(node_replicas);
    // 2. calculate number of replicas per node leading to even replica per
    // node distribution
    auto target_replicas_per_node = total_replicas
                                    / allocator.state().available_nodes();
    vlog(
      clusterlog.info,
      "[update: {}] there are {} replicas in {} domain, requested to assign {} "
      "replicas per node",
      meta.update,
      total_replicas,
      domain,
      target_replicas_per_node);
    // 3. calculate how many replicas have to be moved from each node
    absl::flat_hash_map<model::node_id, size_t> to_move_from_node;

    for (auto& [id, info] : node_replicas) {
        auto to_move = info.allocated_replicas
                       - std::min(
                         target_replicas_per_node, info.allocated_replicas);
        if (to_move > 0) {
            to_move_from_node.emplace(id, to_move);
        }
    }

    if (clusterlog.is_enabled(ss::log_level::info)) {
        for (const auto& [id, cnt] : to_move_from_node) {
            vlog(
              clusterlog.info,
              "[update: {}] there are {} replicas to move from node {} in "
              "domain {}, current allocations: {}",
              meta.update,
              cnt,
              id,
              domain,
              node_replicas[id].allocated_replicas);
        }
    }

    if (to_move_from_node.empty()) {
        return;
    }

    auto to_move_it = to_move_from_node.begin();
    absl::node_hash_set<model::ntp> scheduled_moves;
    while (to_move_it != to_move_from_node.end()) {
        std::vector<partition_reallocation> reallocations;
        auto& [id, to_move] = *to_move_it;

        size_t effective_batch_size = std::min<size_t>(
          to_move, max_batch_size - meta.partition_reallocations.size());

        if (effective_batch_size <= 0) {
            return;
        }
        reallocations.reserve(effective_batch_size);

        size_t idx = 0;
        for (auto& [tp_ns, metadata] : topics.topics_map()) {
            // skip partitions outside of current domain
            if (get_allocation_domain(tp_ns) != domain) {
                continue;
            }
            // do not try to move internal partitions
            if (
              tp_ns.ns == model::kafka_internal_namespace
              || tp_ns.ns == model::redpanda_ns) {
                continue;
            }
            if (!metadata.is_topic_replicable()) {
                continue;
            }
            // do not move topics that were created after node was added, they
            // are allocated with new cluster capacity
            if (meta.update) {
                if (metadata.get_revision() > meta.update->offset()) {
                    vlog(
                      clusterlog.debug,
                      "skipping reallocating topic {}, its revision {} is "
                      "greater than node update {}",
                      tp_ns,
                      metadata.get_revision(),
                      meta.update->offset);
                    continue;
                }
            }

            for (const auto& p : metadata.get_assignments()) {
                if (is_in_replica_set(p.replicas, id)) {
                    partition_reallocation reallocation(
                      model::ntp(tp_ns.ns, tp_ns.tp, p.id), p.replicas.size());
                    if (scheduled_moves.contains(reallocation.ntp)) {
                        continue;
                    }
                    reallocation.replicas_to_remove.emplace(id);
                    reassign_replicas(
                      reallocation.ntp, allocator, p, reallocation);

                    if (!reallocation.allocation_units.has_value()) {
                        continue;
                    }
                    const auto& new_assignment = reallocation.allocation_units
                                                   .value()
                                                   .get_assignments()
                                                   .begin()
                                                   ->replicas;
                    // skip if partition was reassigned to the same node
                    if (is_reassigned_to_node(reallocation, id)) {
                        continue;
                    }
                    const auto added_replicas = subtract_replica_sets(
                      new_assignment, p.replicas);
                    const auto not_improving_move = std::any_of(
                      added_replicas.begin(),
                      added_replicas.end(),
                      [&to_move_from_node](const model::broker_shard& bs) {
                          auto it = to_move_from_node.find(bs.node_id);
                          return it != to_move_from_node.end()
                                 && it->second > 0;
                      });
                    vlog(
                      clusterlog.trace,
                      "ntp {} move from {} -> {} skipping: {}",
                      reallocation.ntp,
                      p.replicas,
                      new_assignment,
                      not_improving_move);

                    /**
                     * If there is no error improvement, skip this reallocation
                     */
                    if (not_improving_move) {
                        continue;
                    }

                    idx++;
                    reallocation.current_replica_set = p.replicas;
                    reallocation.state = reallocation_state::reassigned;
                    /**
                     * Reservoir sampling part, after we have at least required
                     * number of moves replace one of the reallocations with
                     * decreasing probability.
                     */

                    if (reallocations.size() < effective_batch_size) {
                        to_move--;
                        reallocations.push_back(std::move(reallocation));
                    } else {
                        auto r_idx = random_generators::get_int<size_t>(
                          0, idx - 1);
                        if (r_idx < reallocations.size()) {
                            to_move--;
                            std::swap(reallocations[r_idx], reallocation);
                            // update to move as we replaced one of the entries
                            for (const auto node_id :
                                 reallocation.replicas_to_remove) {
                                auto [it, _] = to_move_from_node.try_emplace(
                                  node_id, 0);
                                it->second++;
                            }
                        }
                    }
                }
            }
        }
        ++to_move_it;

        for (auto& r : reallocations) {
            scheduled_moves.emplace(r.ntp);
        }

        std::move(
          reallocations.begin(),
          reallocations.end(),
          std::back_inserter(meta.partition_reallocations));
    }
    if (clusterlog.is_enabled(ss::log_level::debug)) {
        for (auto& r : meta.partition_reallocations) {
            vlog(
              clusterlog.debug,
              "{} moving {} -> {}",
              r.ntp,
              *r.replicas_to_remove.begin(),
              subtract_replica_sets(r.new_replica_set, r.current_replica_set));
        }
    }
}

std::vector<model::ntp> members_backend::ntps_moving_from_node_older_than(
  model::node_id node, model::revision_id revision) const {
    std::vector<model::ntp> ret;

    for (const auto& [ntp, state] : _topics.local().in_progress_updates()) {
        if (state.get_update_revision() < revision) {
            continue;
        }
        if (!contains_node(state.get_previous_replicas(), node)) {
            continue;
        }

        auto current_assignment = _topics.local().get_partition_assignment(ntp);
        if (unlikely(!current_assignment)) {
            continue;
        }

        if (!contains_node(current_assignment->replicas, node)) {
            ret.push_back(ntp);
        }
    }
    return ret;
}

ss::future<> members_backend::calculate_reallocations_after_recommissioned(
  update_meta& meta) {
    vassert(
      meta.update,
      "recommissioning rebalance must be related with node update");
    vassert(
      meta.decommission_update_revision,
      "Decommission update revision must be present for recommission update "
      "metadata");

    auto ntps = ntps_moving_from_node_older_than(
      meta.update->id, meta.decommission_update_revision.value());
    // reallocate all partitions for which any of replicas is placed on
    // decommissioned node
    meta.partition_reallocations.reserve(ntps.size());
    for (auto& ntp : ntps) {
        partition_reallocation reallocation(ntp);
        reallocation.state = reallocation_state::request_cancel;
        auto current_assignment = _topics.local().get_partition_assignment(ntp);
        auto previous_replica_set = _topics.local().get_previous_replica_set(
          ntp);
        if (
          !current_assignment.has_value()
          || !previous_replica_set.has_value()) {
            continue;
        }
        reallocation.current_replica_set = std::move(
          current_assignment->replicas);
        reallocation.new_replica_set = std::move(*previous_replica_set);

        meta.partition_reallocations.push_back(std::move(reallocation));
    }
    co_return;
}

ss::future<std::error_code> members_backend::reconcile() {
    // if nothing to do, wait
    co_await _new_updates.wait([this] { return !_updates.empty(); });
    auto u = co_await _lock.get_units();

    // remove finished updates
    std::erase_if(
      _updates, [](const update_meta& meta) { return meta.finished; });

    if (_updates.empty()) {
        co_return errc::success;
    }

    if (!_raft0->is_elected_leader()) {
        co_return errc::not_leader;
    }

    // use linearizable barrier to make sure leader is up to date and all
    // changes are applied
    auto barrier_result = co_await _raft0->linearizable_barrier();
    if (
      barrier_result.has_error()
      || barrier_result.value() < _raft0->dirty_offset()) {
        if (!barrier_result) {
            vlog(
              clusterlog.debug,
              "error waiting for all raft0 updates to be applied - {}",
              barrier_result.error().message());
            co_return barrier_result.error();
        } else {
            vlog(
              clusterlog.trace,
              "waiting for all raft0 updates to be applied - barrier "
              "offset: "
              "{}, raft_0 dirty offset: {}",
              barrier_result.value(),
              _raft0->dirty_offset());
        }
        co_return errc::not_leader;
    }

    // process one update at a time
    auto& meta = _updates.front();

    // leadership changed, drop not yet requested reallocations to make sure
    // there is no stale state
    auto current_term = _raft0->term();
    if (_last_term != current_term) {
        for (auto& reallocation : meta.partition_reallocations) {
            if (reallocation.state == reallocation_state::reassigned) {
                reallocation.release_assignment_units();
                reallocation.new_replica_set.clear();
                reallocation.state = reallocation_state::initial;
            }
        }
    }
    _last_term = current_term;

    co_await try_to_finish_update(meta);

    vlog(
      clusterlog.info,
      "[update: {}] reconciliation loop - pending reallocation count: {}, "
      "finished: {}",
      meta.update,
      meta.partition_reallocations.size(),
      meta.finished);

    // if update is finished, exit early
    if (meta.finished) {
        co_return errc::success;
    }

    // calculate necessary reallocations
    if (meta.partition_reallocations.empty()) {
        co_await calculate_reallocations(meta);
        // if there is nothing to reallocate, just finish this update
        vlog(
          clusterlog.info,
          "[update: {}] calculated reallocations: {}",
          meta.update,
          meta.partition_reallocations);
        if (should_stop_rebalancing_update(meta)) {
            if (meta.update) {
                auto err = co_await _members_frontend.local()
                             .finish_node_reallocations(meta.update->id);
                if (err) {
                    vlog(
                      clusterlog.info,
                      "[update: {}] reconciliation loop - error finishing "
                      "update - {}",
                      meta.update,
                      err.message());
                    co_return err;
                }
            }
            meta.finished = true;

            vlog(
              clusterlog.debug,
              "[update: {}] no need reallocations, finished: {}",
              meta.update,
              meta.finished);
            co_return errc::success;
        }
    }

    // execute reallocations
    co_await ss::parallel_for_each(
      meta.partition_reallocations,
      [this](partition_reallocation& reallocation) {
          return reallocate_replica_set(reallocation);
      });

    // remove those decommissioned nodes which doesn't have any pending
    // reallocations
    if (
      meta.update
      && meta.update->type
           == members_manager::node_update_type::decommissioned) {
        auto node = _members.local().get_broker(meta.update->id);
        if (!node) {
            vlog(
              clusterlog.debug,
              "reconcile: node {} is gone, returning",
              meta.update->id);
            co_return errc::success;
        }
        const auto is_draining = node.value()->get_membership_state()
                                 == model::membership_state::draining;
        const auto all_reallocations_finished = std::all_of(
          meta.partition_reallocations.begin(),
          meta.partition_reallocations.end(),
          [](const partition_reallocation& r) {
              return r.state == reallocation_state::finished;
          });
        const bool updates_in_progress
          = _topics.local().has_updates_in_progress();

        const auto allocator_empty = _allocator.local().is_empty(
          meta.update->id);
        if (
          is_draining && all_reallocations_finished && allocator_empty
          && !updates_in_progress) {
            // we can safely discard the result since action is going to be
            // retried if it fails
            vlog(
              clusterlog.info,
              "[update: {}] decommissioning finished, removing node from "
              "cluster",
              meta.update);
            // workaround: https://github.com/redpanda-data/redpanda/issues/891
            std::vector<model::node_id> ids{meta.update->id};
            co_await _raft0
              ->remove_members(std::move(ids), model::revision_id{0})
              .discard_result();
        } else {
            // Decommissioning still in progress
            vlog(
              clusterlog.info,
              "[update: {}] decommissioning in progress. draining: {} "
              "all_reallocations_finished: {}, allocator_empty: {} "
              "updates_in_progress:{}",
              meta.update,
              is_draining,
              all_reallocations_finished,
              allocator_empty,
              updates_in_progress);
            if (!allocator_empty && all_reallocations_finished) {
                // recalculate reallocations
                vlog(
                  clusterlog.info,
                  "[update: {}] decommissioning in progress. recalculating "
                  "reallocations",
                  meta.update);
                co_await calculate_reallocations(meta);
            }
        }
    }
    // remove finished reallocations
    std::erase_if(
      meta.partition_reallocations, [](const partition_reallocation& r) {
          return r.state == reallocation_state::finished;
      });

    co_return errc::update_in_progress;
}

bool members_backend::should_stop_rebalancing_update(
  const members_backend::update_meta& meta) const {
    // do not finish decommissioning and recommissioning updates as they have
    // strict stop conditions
    using update_t = members_manager::node_update_type;
    if (
      meta.update
      && (meta.update->type == update_t::decommissioned || meta.update->type == update_t::reallocation_finished)) {
        return false;
    }

    return meta.partition_reallocations.empty();
}

ss::future<>
members_backend::try_to_finish_update(members_backend::update_meta& meta) {
    // broker was removed, finish
    if (meta.update && !_members.local().contains(meta.update->id)) {
        meta.finished = true;
    }

    // topic was removed, mark reallocation as finished
    for (auto& reallocation : meta.partition_reallocations) {
        if (!_topics.local().contains(
              model::topic_namespace_view(reallocation.ntp),
              reallocation.ntp.tp.partition)) {
            reallocation.state = reallocation_state::finished;
        }
    }
    // we do not have to check if all reallocations are finished, we will finish
    // the update when node will be removed
    if (
      meta.update
      && meta.update->type
           == members_manager::node_update_type::decommissioned) {
        co_return;
    }

    // if all reallocations are propagate reallocation finished event and
    // mark update as finished
    const auto all_reallocations_finished = std::all_of(
      meta.partition_reallocations.begin(),
      meta.partition_reallocations.end(),
      [](const partition_reallocation& r) {
          return r.state == reallocation_state::finished;
      });

    if (all_reallocations_finished && !meta.partition_reallocations.empty()) {
        // do not replicate finished update command for on demand
        // reconfiguration
        if (!meta.update) {
            meta.finished = true;
            co_return;
        }
        auto err = co_await _members_frontend.local().finish_node_reallocations(
          meta.update->id);
        if (!err) {
            meta.finished = true;
        }
    }
}

ss::future<> members_backend::reallocate_replica_set(
  members_backend::partition_reallocation& meta) {
    auto current_assignment = _topics.local().get_partition_assignment(
      meta.ntp);
    // topic was deleted, we are done with reallocation
    if (!current_assignment) {
        meta.state = reallocation_state::finished;
        co_return;
    }

    switch (meta.state) {
    case reallocation_state::initial: {
        meta.current_replica_set = current_assignment->replicas;
        // initial state, try to reassign partition replicas

        reassign_replicas(
          meta.ntp, _allocator.local(), std::move(*current_assignment), meta);
        if (meta.new_replica_set.empty()) {
            // if partition allocator failed to reassign partitions return
            // and wait for next retry
            co_return;
        }
        // success, update state and move on
        meta.state = reallocation_state::reassigned;
        vlog(
          clusterlog.info,
          "[ntp: {}, {} -> {}] new partition assignment calculated "
          "successfully",
          meta.ntp,
          meta.current_replica_set,
          meta.new_replica_set);
        [[fallthrough]];
    }
    case reallocation_state::reassigned: {
        vassert(
          !meta.new_replica_set.empty(),
          "reallocation meta in reassigned state must have new_assignment");
        vlog(
          clusterlog.info,
          "[ntp: {}, {} -> {}] dispatching request to move partition",
          meta.ntp,
          meta.current_replica_set,
          meta.new_replica_set);
        // request topic partition move
        std::error_code error
          = co_await _topics_frontend.local().move_partition_replicas(
            meta.ntp,
            meta.new_replica_set,
            model::timeout_clock::now() + _retry_timeout);
        if (error) {
            vlog(
              clusterlog.info,
              "[ntp: {}, {} -> {}] partition move error: {}",
              meta.ntp,
              meta.current_replica_set,
              meta.new_replica_set,
              error.message());
            if (error == errc::update_in_progress) {
                // Skip meta for this partition as it as already moving
                meta.state = reallocation_state::finished;
            }
            co_return;
        }
        // success, update state and move on
        meta.state = reallocation_state::requested;
        meta.release_assignment_units();
        [[fallthrough]];
    }
    case reallocation_state::requested: {
        // wait for partition replicas to be moved
        auto reconciliation_state
          = co_await _api.local().get_reconciliation_state(meta.ntp);
        vlog(
          clusterlog.info,
          "[ntp: {}, {} -> {}] reconciliation state: {}, pending "
          "operations: "
          "{}",
          meta.ntp,
          meta.current_replica_set,
          meta.new_replica_set,
          reconciliation_state.status(),
          reconciliation_state.pending_operations());
        if (reconciliation_state.status() != reconciliation_status::done) {
            co_return;
        }
        meta.state = reallocation_state::finished;
        [[fallthrough]];
    }
    case reallocation_state::finished:
        co_return;
    case reallocation_state::request_cancel: {
        std::error_code error
          = co_await _topics_frontend.local().cancel_moving_partition_replicas(
            meta.ntp, model::timeout_clock::now() + _retry_timeout);
        if (error) {
            vlog(
              clusterlog.info,
              "[ntp: {}, {} -> {}] partition reconfiguration cancellation "
              "error: {}",
              meta.ntp,
              meta.current_replica_set,
              meta.new_replica_set,
              error.message());
            if (error == errc::no_update_in_progress) {
                // mark reallocation as finished, reallocations will be
                // recalculated if required
                meta.state = reallocation_state::finished;
            }
            co_return;
        }
        // success, update state and move on
        meta.state = reallocation_state::cancelled;
        [[fallthrough]];
    }
    case reallocation_state::cancelled: {
        auto reconciliation_state
          = co_await _api.local().get_reconciliation_state(meta.ntp);
        vlog(
          clusterlog.info,
          "[ntp: {}, {} -> {}] reconciliation state: {}, pending "
          "operations: "
          "{}",
          meta.ntp,
          meta.current_replica_set,
          meta.new_replica_set,
          reconciliation_state.status(),
          reconciliation_state.pending_operations());
        if (reconciliation_state.status() != reconciliation_status::done) {
            co_return;
        }
        meta.state = reallocation_state::finished;
        co_return;
    };
    }
}

model::revision_id
members_backend::handle_recommissioned_and_get_decomm_revision(
  members_manager::node_update& update) {
    if (!_members.local().contains(update.id)) {
        return model::revision_id{};
    }
    // find related decommissioning update
    auto it = std::find_if(
      _updates.begin(), _updates.end(), [id = update.id](update_meta& meta) {
          return meta.update && meta.update->id == id
                 && meta.update->type
                      == members_manager::node_update_type::decommissioned;
      });
    vassert(
      it != _updates.end(),
      "decommissioning update must still be present as node {} was not yet "
      "removed from the cluster and it is being decommissioned",
      update.id);
    model::revision_id decommission_rev(it->update->offset);
    // erase related decommissioning update
    _updates.erase(it);

    return decommission_rev;
}

void members_backend::stop_node_decommissioning(model::node_id id) {
    if (_members.local().get_broker(id) == std::nullopt) {
        return;
    }
    // remove all pending decommissioned updates for this node
    std::erase_if(_updates, [id](update_meta& meta) {
        return meta.update && meta.update->id == id
               && meta.update->type
                    == members_manager::node_update_type::decommissioned;
    });
}

void members_backend::stop_node_addition_and_ondemand_rebalance(
  model::node_id id) {
    using update_t = members_manager::node_update_type;
    // remove all pending added updates for current node
    std::erase_if(_updates, [id](update_meta& meta) {
        return !meta.update
               || (meta.update->id == id && meta.update->type == update_t::added);
    });

    if (!_updates.empty()) {
        auto& current = _updates.front();
        if (!current.update || current.update->type == update_t::added) {
            /**
             * Clear current reallocations related with node addition or on
             * demand rebalancing, scheduled reallocations may never finish
             * as the target node may be the one that is decommissioned.
             *
             * Clearing reallocations will force recalculation of
             * reallocations when the update will be processed again, after
             * finishing decommission.
             */

            _updates.front().partition_reallocations.clear();
        }
    }
    // sort updates to prioritize decommissions/recommissions over node
    // additions, use stable sort to keep de/recommissions order
    static auto is_de_or_recommission = [](const update_meta& meta) {
        if (!meta.update.has_value()) {
            return false;
        }
        return meta.update->type == update_t::decommissioned
               || meta.update->type == update_t::recommissioned;
    };

    std::stable_sort(
      _updates.begin(),
      _updates.end(),
      [](const update_meta& lhs, const update_meta& rhs) {
          return is_de_or_recommission(lhs) && !is_de_or_recommission(rhs);
      });
}

void members_backend::handle_reallocation_finished(model::node_id id) {
    // remove all pending added node updates for this node
    std::erase_if(_updates, [id](update_meta& meta) {
        return meta.update && meta.update->id == id
                 && (meta.update->type
                      == members_manager::node_update_type::added
               || meta.update->type
                    == members_manager::node_update_type::recommissioned);
    });
}

std::ostream&
operator<<(std::ostream& o, const members_backend::partition_reallocation& r) {
    fmt::print(
      o,
      "{{ntp: {}, constraints: {},  allocated: {}, state: "
      "{},replicas_to_remove: [",
      r.ntp,
      r.constraints,
      !r.new_replica_set.empty(),
      r.state);

    if (!r.replicas_to_remove.empty()) {
        auto it = r.replicas_to_remove.begin();
        fmt::print(o, "{}", *it);
        ++it;
        for (; it != r.replicas_to_remove.end(); ++it) {
            fmt::print(o, ", {}", *it);
        }
    }
    fmt::print(o, "]}}");
    return o;
}
std::ostream&
operator<<(std::ostream& o, const members_backend::reallocation_state& state) {
    switch (state) {
    case members_backend::reallocation_state::initial:
        return o << "initial";
    case members_backend::reallocation_state::reassigned:
        return o << "reassigned";
    case members_backend::reallocation_state::requested:
        return o << "requested";
    case members_backend::reallocation_state::finished:
        return o << "finished";
    case members_backend::reallocation_state::request_cancel:
        return o << "request_cancel";
    case members_backend::reallocation_state::cancelled:
        return o << "cancelled";
    }

    __builtin_unreachable();
}
} // namespace cluster
