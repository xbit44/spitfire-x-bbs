// Defensive policy model only; this is not a launch primitive or trust store.
#pragma once
#include <array>
#include <cstdint>
namespace final_gate_test {
struct MediaObservation {
    bool independent_package_hash = false;
    bool bounded_exact_inventory = false;
    bool manifest_matches_release = false;
    bool all_payload_hashes_match = false;
    bool no_adopted_program_input = false;
    bool exact_image_handle_retained = false;
    bool ancestor_object_binding = false;
    bool loader_inputs_closed = false;
    bool verifier_alive_through_handoff = false;
    bool elevated_image_independently_bound = false;
};
inline bool may_privileged_use(const MediaObservation& o) {
    return o.independent_package_hash && o.bounded_exact_inventory &&
        o.manifest_matches_release && o.all_payload_hashes_match &&
        o.no_adopted_program_input && o.exact_image_handle_retained &&
        o.ancestor_object_binding && o.loader_inputs_closed &&
        o.verifier_alive_through_handoff && o.elevated_image_independently_bound;
}
enum class Phase { absent, fresh_intent, committed, prepared_update, prepared_rollback, retired, corrupt };
enum class Pair { absent, old_pair, new_pair, mixed, unexpected };
enum class State { intact_prior, verified_new, preserved_source, incomplete_refusal };
struct DurableObservation {
    Phase receipt = Phase::absent;
    Pair pair = Pair::absent;
    bool trusted_receipt_store = false;
    bool exact_binding = false;
    bool exact_security = false;
    bool source_preserved = false;
    bool import_finished = false;
    unsigned committed_generation = 0;
};
inline State recover(const DurableObservation& o) {
    // This is classification, not permission to repair, activate or execute.
    if (o.receipt == Phase::absent && o.pair == Pair::absent && o.source_preserved)
        return State::preserved_source;
    if (!o.trusted_receipt_store || !o.exact_binding || !o.exact_security ||
        !o.import_finished || o.receipt != Phase::committed)
        return State::incomplete_refusal;
    if (o.committed_generation == 1 && o.pair == Pair::old_pair) return State::intact_prior;
    if (o.committed_generation == 2 && o.pair == Pair::new_pair) return State::verified_new;
    return State::incomplete_refusal;
}
struct FreshFacts {
    bool fresh_creation_event = false;
    bool final_descriptors_at_creation = false;
    bool no_retained_ordinary_authority = false;
    bool known_bound_root_bin_files = false;
    bool trusted_media = false;
    bool ordinary_import_complete = false;
    bool final_program_and_data_verification = false;
};
inline bool may_first_commit(const FreshFacts& f) {
    return f.fresh_creation_event && f.final_descriptors_at_creation &&
        f.no_retained_ordinary_authority && f.known_bound_root_bin_files &&
        f.trusted_media && f.ordinary_import_complete && f.final_program_and_data_verification;
}
}
