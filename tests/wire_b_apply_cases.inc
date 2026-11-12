// wire_b_apply_cases.inc - the wire-b lane's cases for tests/test_apply_command.cpp (included once, from that file, after its helpers: covering, refused, ok, ints, num,
// text, A, AppWindow). Every operation of the lane's block that the engine implements has a case here, so the "every op has a case" check sees them; the behaviour
// itself is each op's own tests/test_engine_api_wireb_*.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

void wireBOps(AppWindow& a) {
    // FIELD STRENGTH (FIELD_TABLE / FIELD_CONFIG / FIELD_READ): the window's radio is on the generator; no table is loaded at first.
    covering(FOXAPP_OP_FIELD_READ);
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_FIELD_READ, 0)), FOXAPI_FAILED));
    covering(FOXAPP_OP_FIELD_TABLE);
    CHECK(refused(A::apply(a, text(FOXAPP_OP_FIELD_TABLE, "50,10\n40,20\n", 0, 2)), FOXAPI_BAD_ARGUMENT));   // not increasing
    CHECK(refused(A::apply(a, text(FOXAPP_OP_FIELD_TABLE, "50,10\n", 2, 2)), FOXAPI_OUT_OF_RANGE));         // no such table
    CHECK(ok(A::apply(a, text(FOXAPP_OP_FIELD_TABLE, "50,10\n150,30\n", 0, 2))));
    CHECK(refused(A::apply(a, text(FOXAPP_OP_FIELD_TABLE, "", 1, 2)), FOXAPI_NO_CHANGE));                   // the cable table is empty already
    CHECK(ok(A::apply(a, text(FOXAPP_OP_FIELD_TABLE, "", 0, 2))));
    covering(FOXAPP_OP_FIELD_CONFIG);
    CHECK(refused(A::apply(a, cmd::makeNumInt(FOXAPP_OP_FIELD_CONFIG, 200.0, 0)), FOXAPI_OUT_OF_RANGE));
}
