#pragma once

#include <map>
#include <string>

#include <common_types.hpp>
#include <mechanism.hpp>
#include <memory/memory.hpp>

#include "matrix_state_interleaved.hpp"
#include "matrix_state_flat.hpp"
#include "stimulus.hpp"
#include "threshold_watcher.hpp"

namespace nest {
namespace mc {
namespace gpu {

struct backend {
    /// define the real and index types
    using value_type = double;
    using size_type  = nest::mc::cell_lid_type;

    /// define storage types
    using array  = memory::device_vector<value_type>;
    using iarray = memory::device_vector<size_type>;

    using view       = typename array::view_type;
    using const_view = typename array::const_view_type;

    using iview       = typename iarray::view_type;
    using const_iview = typename iarray::const_view_type;

    using host_array  = typename memory::host_vector<value_type>;
    using host_iarray = typename memory::host_vector<size_type>;

    using host_view   = typename host_array::view_type;
    using host_iview  = typename host_iarray::const_view_type;

    static std::string name() {
        return "gpu";
    }

    // matrix back end implementation
    using matrix_state = matrix_state_interleaved<value_type, size_type>;

    // mechanism infrastructure
    using ion = mechanisms::ion<backend>;

    using mechanism = mechanisms::mechanism_ptr<backend>;

    using stimulus = mechanisms::gpu::stimulus<backend>;

    static mechanism make_mechanism(
        const std::string& name,
        view vec_v, view vec_i,
        const std::vector<value_type>& weights,
        const std::vector<size_type>& node_indices)
    {
        if (!has_mechanism(name)) {
            throw std::out_of_range("no mechanism in database : " + name);
        }

        return mech_map_.find(name)->
            second(vec_v, vec_i, memory::make_const_view(weights), memory::make_const_view(node_indices));
    }

    static bool has_mechanism(const std::string& name) {
        return mech_map_.count(name)>0;
    }

    using threshold_watcher =
        nest::mc::gpu::threshold_watcher<value_type, size_type>;

private:

    using maker_type = mechanism (*)(view, view, array&&, iarray&&);
    static std::map<std::string, maker_type> mech_map_;

    template <template <typename> class Mech>
    static mechanism maker(view vec_v, view vec_i, array&& weights, iarray&& node_indices) {
        return mechanisms::make_mechanism<Mech<backend>>
            (vec_v, vec_i, std::move(weights), std::move(node_indices));
    }
};

} // namespace gpu
} // namespace mc
} // namespace nest