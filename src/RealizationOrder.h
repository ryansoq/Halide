#ifndef HALIDE_INTERNAL_REALIZATION_ORDER_H
#define HALIDE_INTERNAL_REALIZATION_ORDER_H

/** \file
 *
 * Defines the lowering pass that determines the order in which
 * realizations are injected and groups functions with fused
 * computation loops.
 */

#include <map>
#include <string>
#include <vector>

namespace Halide {
namespace Internal {

class Function;
struct FusedStageContents;
struct FusedGroupContents;

class FusedStage {
    IntrusivePtr<FusedStageContents> contents;

public:
    FusedStage();
    FusedStage(const FusedStage &other) : contents(other.contents) {}
    FusedStage(IntrusivePtr<FusedStageContents> ptr) : contents(std::move(ptr)) {}

    friend struct std::hash<Halide::Internal::FusedStage>;

    bool operator==(const FusedStage &other) const;
};

class FusedGroup {
    IntrusivePtr<FusedGroupContents> contents;

public:
    FusedGroup();
    FusedGroup(const FusedGroup &other) : contents(other.contents) {}
    FusedGroup(IntrusivePtr<FusedGroupContents> ptr) : contents(std::move(ptr)) {}

    friend struct std::hash<Halide::Internal::FusedGroup>;

    bool operator==(const FusedGroup &other) const;
    void add_stage(const FusedStage &stage);
};

/** Given a bunch of functions that call each other, determine an
 * order in which to do the scheduling. This in turn influences the
 * order in which stages are computed when there's no strict
 * dependency between them. Currently just some arbitrary depth-first
 * traversal of the call graph. In addition, determine grouping of functions
 * with fused computation loops. The functions within the fused groups
 * are sorted based on realization order. There should not be any dependencies
 * among functions within a fused group. This pass will also populate the
 * 'fused_pairs' list in the function's schedule. Return a pair of
 * the realization order and the fused groups in that order.
 */
std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>> realization_order(
    const std::vector<Function> &outputs, std::map<std::string, Function> &env);

/** Given a bunch of functions that call each other, determine a
 * topological order which stays constant regardless of the schedule.
 * This ordering adheres to the producer-consumer dependencies, i.e. producer
 * will come before its consumers in that order */
std::vector<std::string> topological_order(
        const std::vector<Function> &outputs, const std::map<std::string, Function> &env);

}  // namespace Internal
}  // namespace Halide

namespace std {
template <> struct hash<Halide::Internal::FusedStage>;
template <> struct hash<Halide::Internal::FusedGroup>;
}

#endif
