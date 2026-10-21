#pragma once

namespace wqn {

// Boot-time self-test for the §4.2 commit_state gates: navigation away from a
// scoped word page, an active problem verdict, the [词]-row scope switch, the
// settings-page Confirm and the factory-reset dialog must all refuse while a
// local write is armed or in flight.
//
// Runs from RunContractFixtureSelfTest (main.cpp:190), which is BEFORE the UI
// task and the persist worker start. That is what makes it safe: every case
// asserts a REFUSAL, and a refusal performs no storage work at all. The same
// ordering is also the limit of what this can cover -- it exercises the pure
// reducer only, never the worker.
//
// A failure is not fatal to boot (the caller logs one line and continues), but
// each case logs its own name so the boot log says which gate broke.
bool RunUiGateSelfTest();

}  // namespace wqn
