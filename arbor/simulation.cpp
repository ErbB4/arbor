#include <memory>
#include <set>
#include <vector>

#include <arbor/arbexcept.hpp>
#include <arbor/context.hpp>
#include <arbor/domain_decomposition.hpp>
#include <arbor/generic_event.hpp>
#include <arbor/recipe.hpp>
#include <arbor/schedule.hpp>
#include <arbor/simulation.hpp>

#include "cell_group.hpp"
#include "cell_group_factory.hpp"
#include "communication/communicator.hpp"
#include "execution_context.hpp"
#include "merge_events.hpp"
#include "thread_private_spike_store.hpp"
#include "threading/threading.hpp"
#include "util/filter.hpp"
#include "util/maputil.hpp"
#include "util/partition.hpp"
#include "util/span.hpp"
#include "profile/profiler_macro.hpp"

namespace arb {

template <typename Seq, typename Value, typename Less = std::less<>>
auto split_sorted_range(Seq&& seq, const Value& v, Less cmp = Less{}) {
    auto canon = util::canonical_view(seq);
    auto it = std::lower_bound(canon.begin(), canon.end(), v, cmp);
    return std::make_pair(
        util::make_range(seq.begin(), it),
        util::make_range(it, seq.end()));
}

// Create a new cell event_lane vector from sorted pending events, previous event_lane events,
// and events from event generators for the given interval.
void merge_cell_events(
    time_type t_from,
    time_type t_to,
    event_span old_events,
    event_span pending,
    std::vector<event_generator>& generators,
    pse_vector& new_events)
{
    PE(communication:enqueue:setup);
    new_events.clear();
    old_events = split_sorted_range(old_events, t_from, event_time_less()).second;
    PL();

    if (!generators.empty()) {
        PE(communication:enqueue:setup);
        // Tree-merge events in [t_from, t_to) from old, pending and generator events.

        std::vector<event_span> spanbuf;
        spanbuf.reserve(2+generators.size());

        auto old_split = split_sorted_range(old_events, t_to, event_time_less());
        auto pending_split = split_sorted_range(pending, t_to, event_time_less());

        spanbuf.push_back(old_split.first);
        spanbuf.push_back(pending_split.first);

        for (auto& g: generators) {
            event_span evs = g.events(t_from, t_to);
            if (!evs.empty()) {
                spanbuf.push_back(evs);
            }
        }
        PL();

        PE(communication:enqueue:tree);
        tree_merge_events(spanbuf, new_events);
        PL();

        old_events = old_split.second;
        pending = pending_split.second;
    }

    // Merge (remaining) old and pending events.
    PE(communication:enqueue:merge);
    auto n = new_events.size();
    new_events.resize(n+pending.size()+old_events.size());
    std::merge(pending.begin(), pending.end(), old_events.begin(), old_events.end(), new_events.begin()+n);
    PL();
}


class simulation_state {
public:
    simulation_state(const recipe& rec, const domain_decomposition& decomp, execution_context ctx);

    void reset();

    time_type run(time_type tfinal, time_type dt);

    sampler_association_handle add_sampler(cell_member_predicate probe_ids,
        schedule sched, sampler_function f, sampling_policy policy = sampling_policy::lax);

    void remove_sampler(sampler_association_handle);

    void remove_all_samplers();

    std::vector<probe_metadata> get_probe_metadata(cell_member_type) const;

    std::size_t num_spikes() const {
        return communicator_.num_spikes();
    }

    void set_binning_policy(binning_kind policy, time_type bin_interval);

    void inject_events(const cse_vector& events);

    spike_export_function global_export_callback_;
    spike_export_function local_export_callback_;

private:
    // Record last computed epoch (integration interval).
    epoch epoch_;

    // Maximum epoch duration.
    time_type t_interval_ = 0;

    std::vector<cell_group_ptr> cell_groups_;

    // One set of event_generators for each local cell
    std::vector<std::vector<event_generator>> event_generators_;

    // Hash table for looking up the the local index of a cell with a given gid
    struct gid_local_info {
        cell_size_type cell_index;
        cell_size_type group_index;
    };
    std::unordered_map<cell_gid_type, gid_local_info> gid_to_local_;

    communicator communicator_;

    task_system_handle task_system_;

    // Pending events to be delivered.
    std::vector<pse_vector> pending_events_;
    std::array<std::vector<pse_vector>, 2> event_lanes_;

    std::vector<pse_vector>& event_lanes(std::ptrdiff_t epoch_id) {
        return event_lanes_[epoch_id&1];
    }

    // Spikes generated by local cell groups.
    std::array<thread_private_spike_store, 2> local_spikes_;

    thread_private_spike_store& local_spikes(std::ptrdiff_t epoch_id) {
        return local_spikes_[epoch_id&1];
    }

    // Sampler associations handles are managed by a helper class.
    util::handle_set<sampler_association_handle> sassoc_handles_;

    // Apply a functional to each cell group in parallel.
    template <typename L>
    void foreach_group(L&& fn) {
        threading::parallel_for::apply(0, cell_groups_.size(), task_system_.get(),
            [&, fn = std::forward<L>(fn)](int i) { fn(cell_groups_[i]); });
    }

    // Apply a functional to each cell group in parallel, supplying
    // the cell group pointer reference and index.
    template <typename L>
    void foreach_group_index(L&& fn) {
        threading::parallel_for::apply(0, cell_groups_.size(), task_system_.get(),
            [&, fn = std::forward<L>(fn)](int i) { fn(cell_groups_[i], i); });
    }

    // Apply a functional to each local cell in parallel.
    template <typename L>
    void foreach_cell(L&& fn) {
        threading::parallel_for::apply(0, communicator_.num_local_cells(), task_system_.get(), fn);
    }
};

simulation_state::simulation_state(
        const recipe& rec,
        const domain_decomposition& decomp,
        execution_context ctx
    ):
    task_system_(ctx.thread_pool),
    local_spikes_({thread_private_spike_store(ctx.thread_pool), thread_private_spike_store(ctx.thread_pool)})
{
    // Generate the cell groups in parallel, with one task per cell group.
    cell_groups_.resize(decomp.num_groups());
    std::vector<cell_labels_and_gids> cg_sources(cell_groups_.size());
    std::vector<cell_labels_and_gids> cg_targets(cell_groups_.size());
    foreach_group_index(
        [&](cell_group_ptr& group, int i) {
          const auto& group_info = decomp.group(i);
          cell_label_range sources, targets;
          auto factory = cell_kind_implementation(group_info.kind, group_info.backend, ctx);
          group = factory(group_info.gids, rec, sources, targets);

          cg_sources[i] = cell_labels_and_gids(std::move(sources), group_info.gids);
          cg_targets[i] = cell_labels_and_gids(std::move(targets), group_info.gids);
        });

    cell_labels_and_gids local_sources, local_targets;
    for(const auto& i: util::make_span(cell_groups_.size())) {
        local_sources.append(cg_sources.at(i));
        local_targets.append(cg_targets.at(i));
    }
    auto global_sources = ctx.distributed->gather_cell_labels_and_gids(local_sources);

    auto source_resolution_map = label_resolution_map(std::move(global_sources));
    auto target_resolution_map = label_resolution_map(std::move(local_targets));

    communicator_ = arb::communicator(rec, decomp, source_resolution_map, target_resolution_map, ctx);

    const auto num_local_cells = communicator_.num_local_cells();

    // Use half minimum delay of the network for max integration interval.
    t_interval_ = communicator_.min_delay()/2;

    // Initialize empty buffers for pending events for each local cell
    pending_events_.resize(num_local_cells);

    event_generators_.resize(num_local_cells);
    cell_size_type lidx = 0;
    cell_size_type grpidx = 0;

    auto target_resolution_map_ptr = std::make_shared<label_resolution_map>(std::move(target_resolution_map));
    for (const auto& group_info: decomp.groups()) {
        for (auto gid: group_info.gids) {
            // Store mapping of gid to local cell index.
            gid_to_local_[gid] = gid_local_info{lidx, grpidx};

            // Resolve event_generator targets.
            // Each event generator gets their own resolver state.
            auto event_gens = rec.event_generators(gid);
            for (auto& g: event_gens) {
                g.resolve_label([target_resolution_map_ptr, event_resolver=resolver(target_resolution_map_ptr.get()), gid]
                    (const cell_local_label_type& label) mutable {
                        return event_resolver.resolve({gid, label});
                    });
            }

            // Set up the event generators for cell gid.
            event_generators_[lidx] = event_gens;

            ++lidx;
        }
        ++grpidx;
    }

    // Create event lane buffers.
    // One buffer is consumed by cell group updates while the other is filled with events for
    // the following epoch. In each buffer there is one lane for each local cell.
    event_lanes_[0].resize(num_local_cells);
    event_lanes_[1].resize(num_local_cells);

    epoch_.reset();
}

void simulation_state::reset() {
    epoch_ = epoch();

    // Reset cell group state.
    foreach_group([](cell_group_ptr& group) { group->reset(); });

    // Clear all pending events in the event lanes.
    for (auto& lanes: event_lanes_) {
        for (auto& lane: lanes) {
            lane.clear();
        }
    }

    // Reset all event generators.
    for (auto& lane: event_generators_) {
        for (auto& gen: lane) {
            gen.reset();
        }
    }

    for (auto& lane: pending_events_) {
        lane.clear();
    }

    communicator_.reset();

    for (auto& spikes: local_spikes_) {
        spikes.clear();
    }

    epoch_.reset();
}

time_type simulation_state::run(time_type tfinal, time_type dt) {
    // Progress simulation to time tfinal, through a series of integration epochs
    // of length at most t_interval_. t_interval_ is chosen to be no more than
    // than half the network minimum delay.
    //
    // There are three simulation tasks that can be run partially in parallel:
    //
    // 1. Update:
    //    Ask each cell group to update their state to the end of the integration epoch.
    //    Generated spikes are stored in local_spikes_ for this epoch.
    //
    // 2. Exchange:
    //    Consume local spikes held in local_spikes_ from a previous update, and collect
    //    such spikes from across all ranks.
    //    Translate spikes to local postsynaptic spike events, to be appended to pending_events_.
    //
    // 3. Enqueue events:
    //    Take events from pending_events_, together with any event-generator events for the
    //    next epoch and any left over events from the last epoch, and collate them into
    //    the per-cell event_lanes for the next epoch.
    //
    // Writing U(k) for Update on kth epoch; D(k) for Exchange of spikes generated in the kth epoch;
    // and E(k) for Enqueue of the events required for the kth epoch, there are the following
    // dependencies:
    //
    //     * E(k) precedes U(k).
    //     * U(k) precedes D(k).
    //     * U(k) precedes U(k+1).
    //     * D(k) precedes E(k+2).
    //     * D(k) precedes D(k+1).
    //
    // In the schedule implemented below, U(k) and D(k-1) or U(k) and E(k+1) can be run
    // in parallel, while D and E operations must be serialized (D writes to pending_events_,
    // while E consumes and clears it). The local spike collection and the per-cell event
    // lanes are double buffered.
    //
    // Required state on run() invocation with epoch_.id==k:
    //     * For k≥0,  U(k) and D(k) have completed.
    //
    // Requires state at end of run(), with epoch_.id==k:
    //     * U(k) and D(k) have completed.

    if (tfinal<=epoch_.t1) return epoch_.t1;

    // Compute following epoch, with max time tfinal.
    auto next_epoch = [tfinal](epoch e, time_type interval) -> epoch {
        epoch next = e;
        next.advance_to(std::min(next.t1+interval, tfinal));
        return next;
    };

    // Update task: advance cell groups to end of current epoch and store spikes in local_spikes_.
    auto update = [this, dt](epoch current) {
        local_spikes(current.id).clear();
        foreach_group_index(
            [&](cell_group_ptr& group, int i) {
                auto queues = util::subrange_view(event_lanes(current.id), communicator_.group_queue_range(i));
                group->advance(current, dt, queues);

                PE(advance:spikes);
                local_spikes(current.id).insert(group->spikes());
                group->clear_spikes();
                PL();
            });
    };

    // Exchange task: gather previous locally generated spikes, distribute across all ranks, and deliver
    // post-synaptic spike events to per-cell pending event vectors.
    auto exchange = [this](epoch prev) {
        // Collate locally generated spikes.
        PE(communication:exchange:gatherlocal);
        auto all_local_spikes = local_spikes(prev.id).gather();
        PL();
        // Gather generated spikes across all ranks.
        auto global_spikes = communicator_.exchange(all_local_spikes);

        // Present spikes to user-supplied callbacks.
        PE(communication:spikeio);
        if (local_export_callback_) {
            local_export_callback_(all_local_spikes);
        }
        if (global_export_callback_) {
            global_export_callback_(global_spikes.values());
        }
        PL();

        // Append events formed from global spikes to per-cell pending event queues.
        PE(communication:walkspikes);
        communicator_.make_event_queues(global_spikes, pending_events_);
        PL();
    };

    // Enqueue task: build event_lanes for next epoch from pending events, event-generator events for the
    // next epoch, and with any unprocessed events from the current event_lanes.
    auto enqueue = [this](epoch next) {
        foreach_cell(
            [&](cell_size_type i) {
                PE(communication:enqueue:sort);
                util::sort(pending_events_[i]);
                PL();

                event_span pending = util::range_pointer_view(pending_events_[i]);
                event_span old_events = util::range_pointer_view(event_lanes(next.id-1)[i]);

                merge_cell_events(next.t0, next.t1, old_events, pending, event_generators_[i], event_lanes(next.id)[i]);
                pending_events_[i].clear();
            });
    };

    threading::task_group g(task_system_.get());

    epoch prev = epoch_;
    epoch current = next_epoch(prev, t_interval_);
    epoch next = next_epoch(current, t_interval_);

    if (next.empty()) {
        enqueue(current);
        update(current);
        exchange(current);
    }
    else {
        enqueue(current);

        g.run([&]() { enqueue(next); });
        g.run([&]() { update(current); });
        g.wait();

        for (;;) {
            prev = current;
            current = next;
            next = next_epoch(next, t_interval_);
            if (next.empty()) break;

            g.run([&]() { exchange(prev); enqueue(next); });
            g.run([&]() { update(current); });
            g.wait();
        }

        g.run([&]() { exchange(prev); });
        g.run([&]() { update(current); });
        g.wait();

        exchange(current);
    }

    // Record current epoch for next run() invocation.
    epoch_ = current;
    return current.t1;
}

sampler_association_handle simulation_state::add_sampler(
        cell_member_predicate probe_ids,
        schedule sched,
        sampler_function f,
        sampling_policy policy)
{
    sampler_association_handle h = sassoc_handles_.acquire();

    foreach_group(
        [&](cell_group_ptr& group) { group->add_sampler(h, probe_ids, sched, f, policy); });

    return h;
}

void simulation_state::remove_sampler(sampler_association_handle h) {
    foreach_group(
        [h](cell_group_ptr& group) { group->remove_sampler(h); });

    sassoc_handles_.release(h);
}

void simulation_state::remove_all_samplers() {
    foreach_group(
        [](cell_group_ptr& group) { group->remove_all_samplers(); });

    sassoc_handles_.clear();
}

std::vector<probe_metadata> simulation_state::get_probe_metadata(cell_member_type probe_id) const {
    if (auto linfo = util::value_by_key(gid_to_local_, probe_id.gid)) {
        return cell_groups_.at(linfo->group_index)->get_probe_metadata(probe_id);
    }
    else {
        return {};
    }
}

void simulation_state::set_binning_policy(binning_kind policy, time_type bin_interval) {
    foreach_group(
        [&](cell_group_ptr& group) { group->set_binning_policy(policy, bin_interval); });
}

void simulation_state::inject_events(const cse_vector& events) {
    // Push all events that are to be delivered to local cells into the
    // pending event list for the event's target cell.
    for (auto& [gid, pse_vector]: events) {
        for (auto& e: pse_vector) {
            if (e.time < epoch_.t1) {
                throw bad_event_time(e.time, epoch_.t1);
            }
            // gid_to_local_ maps gid to index in local cells and of corresponding cell group.
            if (auto lidx = util::value_by_key(gid_to_local_, gid)) {
                pending_events_[lidx->cell_index].push_back(e);
            }
        }
    }
}

// Simulation class implementations forward to implementation class.

simulation::simulation(
    const recipe& rec,
    const domain_decomposition& decomp,
    const context& ctx)
{
    impl_.reset(new simulation_state(rec, decomp, *ctx));
}

void simulation::reset() {
    impl_->reset();
}

time_type simulation::run(time_type tfinal, time_type dt) {
    if (dt <= 0.0) {
        throw domain_error("Finite time-step must be supplied.");
    }
    return impl_->run(tfinal, dt);
}

sampler_association_handle simulation::add_sampler(
    cell_member_predicate probe_ids,
    schedule sched,
    sampler_function f,
    sampling_policy policy)
{
    return impl_->add_sampler(std::move(probe_ids), std::move(sched), std::move(f), policy);
}

void simulation::remove_sampler(sampler_association_handle h) {
    impl_->remove_sampler(h);
}

void simulation::remove_all_samplers() {
    impl_->remove_all_samplers();
}

std::vector<probe_metadata> simulation::get_probe_metadata(cell_member_type probe_id) const {
    return impl_->get_probe_metadata(probe_id);
}

std::size_t simulation::num_spikes() const {
    return impl_->num_spikes();
}

void simulation::set_binning_policy(binning_kind policy, time_type bin_interval) {
    impl_->set_binning_policy(policy, bin_interval);
}

void simulation::set_global_spike_callback(spike_export_function export_callback) {
    impl_->global_export_callback_ = std::move(export_callback);
}

void simulation::set_local_spike_callback(spike_export_function export_callback) {
    impl_->local_export_callback_ = std::move(export_callback);
}

void simulation::inject_events(const cse_vector& events) {
    impl_->inject_events(events);
}

simulation::~simulation() = default;

} // namespace arb
