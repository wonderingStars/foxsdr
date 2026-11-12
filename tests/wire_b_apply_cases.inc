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
    // THE SIGNAL MEASUREMENTS: a parameter text that is not a JSON object is refused before anything is read from the radio (the measuring is test_engine_api_wireb_sigmeas's)
    const std::uint32_t sigmeasOps[] = {FOXAPP_OP_SIGMEAS_BANDWIDTH, FOXAPP_OP_SIGMEAS_MODULATION, FOXAPP_OP_SIGMEAS_SYMBOLS, FOXAPP_OP_SIGMEAS_VSTIME, FOXAPP_OP_SIGMEAS_BURSTS,
                                        FOXAPP_OP_SIGMEAS_HOPCHIRP, FOXAPP_OP_SIGMEAS_AUDIO, FOXAPP_OP_SIGMEAS_DRIFT, FOXAPP_OP_SIGMEAS_HISTOGRAM, FOXAPP_OP_PSEUDO_DOPPLER};
    for (const std::uint32_t op : sigmeasOps) {
        covering(op);
        CHECK(refused(A::apply(a, text(op, "not json")), FOXAPI_BAD_ARGUMENT));
    }
    // THE BIT VIEW: parameter text that is not a JSON object is refused first; a decoder or table text goes to its own checker (an error list, not a refusal)
    const std::uint32_t bitsJsonOps[] = {FOXAPP_OP_BITS_KEEP, FOXAPP_OP_BITS_LIST, FOXAPP_OP_BITS_REDECODE, FOXAPP_OP_BITS_SWEEP, FOXAPP_OP_BITS_FIND_CRC, FOXAPP_OP_BITS_FIND_SUM, FOXAPP_OP_BITS_INFER, FOXAPP_OP_BITS_DIFF};
    for (const std::uint32_t op : bitsJsonOps) {
        covering(op);
        CHECK(refused(A::apply(a, text(op, "not json")), FOXAPI_BAD_ARGUMENT));
    }
    for (const std::uint32_t op : {FOXAPP_OP_BITS_SPEC_CHECK, FOXAPP_OP_BITS_SPEC_RUN, FOXAPP_OP_BITS_TABLE_CHECK}) { covering(op); }
    // GPU SPECTRUM: a mode that is not 0, 1 or 2 is refused; the CPU mode is the default, so asking for it is no change (the GPU is never brought up by a test of the window)
    // THE CURSORS AND THE FRAME LOG: bad parameter text is refused first; the frame log has no window state to touch
    for (const std::uint32_t op : {FOXAPP_OP_CURSOR_SET, FOXAPP_OP_CURSOR_READ, FOXAPP_OP_CURSOR_MARKS}) {
        covering(op);
        CHECK(refused(A::apply(a, text(op, "not json")), FOXAPI_BAD_ARGUMENT));
    }
    covering(FOXAPP_OP_FRAMELOG_CONFIG);
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_FRAMELOG_CONFIG, 2, 0)), FOXAPI_OUT_OF_RANGE));
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_FRAMELOG_CONFIG, -1, 0)), FOXAPI_NO_CHANGE));
    covering(FOXAPP_OP_FRAMELOG_READ);
    CHECK(ok(A::apply(a, ints(FOXAPP_OP_FRAMELOG_READ, 1))));
    covering(FOXAPP_OP_GPU_FFT_MODE);
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_GPU_FFT_MODE, 3)), FOXAPI_OUT_OF_RANGE));
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_GPU_FFT_MODE, 0)), FOXAPI_NO_CHANGE));
}
