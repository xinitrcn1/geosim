#include "sim/params.hpp"

namespace geosim::sim {

bool set_param(params& p, std::string_view name, double value) noexcept {
#define GEOSIM_PARAM_SET(n, def, desc) \
    if(name == #n) {                   \
        p.n = value;                   \
        return true;                   \
    }
    GEOSIM_PARAMS(GEOSIM_PARAM_SET)
#undef GEOSIM_PARAM_SET
    return false;
}

double const* find_param(params const& p, std::string_view name) noexcept {
#define GEOSIM_PARAM_FIND(n, def, desc) \
    if(name == #n) {                    \
        return &p.n;                    \
    }
    GEOSIM_PARAMS(GEOSIM_PARAM_FIND)
#undef GEOSIM_PARAM_FIND
    return nullptr;
}

} // namespace geosim::sim
