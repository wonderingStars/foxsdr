// wire_a_apply_cover.inc - test_apply_command's coverage rule ("every op applyCommand implements has a case") for the ops of wiring lane wire-a. Included inside the
// anonymous namespace of test_apply_command.cpp; each op gets one refusal through the real applyCommand here, the behaviour is held by tests/test_engine_api_wire_a_*.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
void wireAOps(AppWindow& a) {
    covering(FOXAPP_OP_HISTORY_STEP);
    {
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_HISTORY_STEP, 2)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_HISTORY_STEP, 0)), FOXAPI_NO_CHANGE));   // nothing to go back to
    }
}
