#pragma once
#include "Database.h"
#include "logging.h"
#include <algorithm>
#include <string_view>
#include <utility>

namespace mailadmin {
// Labels are developer-owned literals, never configuration/input values.
// Numeric identifiers are restricted again before being sent to the logger.
struct ReconciliationLocation {
    std::string_view stage, record, record_id, field;
    std::string_view passwd_id = {}, organization_id = {};
    bool provisioning_started = false;
};
inline std::string_view diagnostic_id(std::string_view value) {
    if (value.empty()) return "none";
    if (value.size() > 20 || !std::all_of(value.begin(),value.end(),[](char c) { return c >= '0' && c <= '9'; }))
        return "invalid";
    return value;
}
inline void report_reconciliation_failure(const ReconciliationLocation& location,
        std::string_view code, std::string_view exception = "none") {
    MA_ERROR << "Reconciliation step failed; stage=" << location.stage
        << " record=" << location.record << " record_id=" << diagnostic_id(location.record_id)
        << " passwd_id=" << diagnostic_id(location.passwd_id)
        << " organization_id=" << diagnostic_id(location.organization_id)
        << " field=" << location.field
        << " provisioning_started=" << (location.provisioning_started ? "yes" : "no")
        << " code=" << code << " exception=" << exception;
}
template<typename Function>
decltype(auto) reconciliation_step(const ReconciliationLocation& location, Function&& function) {
    try { return std::forward<Function>(function)(); }
    catch (const ApiError& error) { report_reconciliation_failure(location,error.code); throw; }
    // Keep numeric conversion exceptions and their behavior unchanged. They have
    // no ApiError code; do not misrepresent them as structured API errors.
    catch (const std::invalid_argument&) { report_reconciliation_failure(location,"none","invalid_argument"); throw; }
    catch (const std::out_of_range&) { report_reconciliation_failure(location,"none","out_of_range"); throw; }
}
}
