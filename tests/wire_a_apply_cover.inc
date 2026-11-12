// wire_a_apply_cover.inc - test_apply_command's coverage rule ("every op applyCommand implements has a case") for the ops of wiring lane wire-a. Included inside the
// anonymous namespace of test_apply_command.cpp; each op gets one refusal through the real applyCommand here, the behaviour is held by tests/test_engine_api_wire_a_*.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
void wireAOps(AppWindow& a) {
    covering(FOXAPP_OP_BOOKMARK_NOTE);
    covering(FOXAPP_OP_BOOKMARK_FIND);
    {
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_BOOKMARK_NOTE, 987654321)), FOXAPI_NOT_FOUND));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_BOOKMARK_FIND, 5)), FOXAPI_OUT_OF_RANGE));
    }
    covering(FOXAPP_OP_MARKER_EXPORT);
    covering(FOXAPP_OP_MARKER_IMPORT);
    {
        CHECK(refused(A::apply(a, text(FOXAPP_OP_MARKER_EXPORT, "relative.csv")), FOXAPI_BAD_ARGUMENT));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_MARKER_IMPORT, "relative.csv")), FOXAPI_BAD_ARGUMENT));
    }
    covering(FOXAPP_OP_CHANNEL);
    {
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_CHANNEL, 9)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_CHANNEL, 1)), FOXAPI_NO_CHANGE));   // no plan chosen
    }
    covering(FOXAPP_OP_HISTORY_STEP);
    {
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_HISTORY_STEP, 2)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_HISTORY_STEP, 0)), FOXAPI_NO_CHANGE));   // nothing to go back to
    }
}
