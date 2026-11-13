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
    // THE LIMIT MASK: a bad file is refused with its line (the old mask stays), nothing to remove is no change, a parameter that is not JSON is refused, enabling with no mask fails
    covering(FOXAPP_OP_MASK_LOAD);
    CHECK(refused(A::apply(a, text(FOXAPP_OP_MASK_LOAD, "foxmask 1\nupper 1 2 3\n")), FOXAPI_BAD_ARGUMENT));
    CHECK(refused(A::apply(a, text(FOXAPP_OP_MASK_LOAD, "")), FOXAPI_NO_CHANGE));
    covering(FOXAPP_OP_MASK_CONFIG);
    CHECK(refused(A::apply(a, text(FOXAPP_OP_MASK_CONFIG, "not json")), FOXAPI_BAD_ARGUMENT));
    CHECK(refused(A::apply(a, text(FOXAPP_OP_MASK_CONFIG, "{\"enabled\":true}")), FOXAPI_FAILED));
    // THE SWEEP DEVICE: a port that is not a COM port name is refused before the system sees it; closing what is not open is no change; reading needs nothing open
    covering(FOXAPP_OP_SWEEP_OPEN);
    CHECK(refused(A::apply(a, text(FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"tinysa\",\"port\":\"COM999\",\"start_mhz\":88,\"stop_mhz\":108}")), FOXAPI_BAD_ARGUMENT));
    covering(FOXAPP_OP_SWEEP_CLOSE);
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_SWEEP_CLOSE, 0)), FOXAPI_NO_CHANGE));
    covering(FOXAPP_OP_SWEEP_CONFIG);
    CHECK(refused(A::apply(a, text(FOXAPP_OP_SWEEP_CONFIG, "not json")), FOXAPI_BAD_ARGUMENT));
    covering(FOXAPP_OP_SWEEP_READ);
    CHECK(refused(A::apply(a, text(FOXAPP_OP_SWEEP_READ, "{}")), FOXAPI_FAILED));
    // NETWORK I/Q OUT: a key that does not exist is refused before any socket is made; nothing to stop is no change
    covering(FOXAPP_OP_IQ_OUT_START);
    CHECK(refused(A::apply(a, text(FOXAPP_OP_IQ_OUT_START, "{\"bind\":\"0.0.0.0\"}")), FOXAPI_BAD_ARGUMENT));
    covering(FOXAPP_OP_IQ_OUT_STOP);
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_IQ_OUT_STOP, 0)), FOXAPI_NO_CHANGE));
    // RADIOS THAT FOLLOW A MASTER: the window has one radio, so a link needs a second one (the engine test does that); the refusals that need none
    covering(FOXAPP_OP_FOLLOW_SET);
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_FOLLOW_SET, 1, 1)), FOXAPI_BAD_ARGUMENT));   // a radio cannot follow itself
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_FOLLOW_SET, 9, 1)), FOXAPI_OUT_OF_RANGE));
    covering(FOXAPP_OP_FOLLOW_CLEAR);
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_FOLLOW_CLEAR, 0)), FOXAPI_NO_CHANGE));
    covering(FOXAPP_OP_FOLLOW_LOCK);
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_FOLLOW_LOCK, 2, 1)), FOXAPI_NO_CHANGE));
    covering(FOXAPP_OP_FOLLOW_KEEP);
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_FOLLOW_KEEP, 2)), FOXAPI_NO_CHANGE));
    // SBS OUT: no key for a wider bind; nothing to stop is no change
    covering(FOXAPP_OP_SBS_OUT_START);
    CHECK(refused(A::apply(a, text(FOXAPP_OP_SBS_OUT_START, "{\"bind\":\"0.0.0.0\"}")), FOXAPI_BAD_ARGUMENT));
    covering(FOXAPP_OP_SBS_OUT_STOP);
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_SBS_OUT_STOP, 0)), FOXAPI_NO_CHANGE));
    // THE TRACK STORE: off by default; a parameter that is not JSON is refused, reading needs nothing held, clearing nothing is no change
    covering(FOXAPP_OP_TRACKS_CONFIG);
    CHECK(refused(A::apply(a, text(FOXAPP_OP_TRACKS_CONFIG, "not json")), FOXAPI_BAD_ARGUMENT));
    CHECK(refused(A::apply(a, text(FOXAPP_OP_TRACKS_CONFIG, "{\"enabled\":false}")), FOXAPI_NO_CHANGE));
    covering(FOXAPP_OP_TRACKS_READ);
    CHECK(refused(A::apply(a, text(FOXAPP_OP_TRACKS_READ, "not json")), FOXAPI_BAD_ARGUMENT));
    // THE COVERAGE GRID: off by default; enabling needs the receiver position from the caller; reading needs it on
    covering(FOXAPP_OP_COVERAGE_CONFIG);
    CHECK(refused(A::apply(a, text(FOXAPP_OP_COVERAGE_CONFIG, "not json")), FOXAPI_BAD_ARGUMENT));
    CHECK(refused(A::apply(a, text(FOXAPP_OP_COVERAGE_CONFIG, "{\"enabled\":true}")), FOXAPI_BAD_ARGUMENT));   // no position given
    covering(FOXAPP_OP_COVERAGE_READ);
    CHECK(refused(A::apply(a, text(FOXAPP_OP_COVERAGE_READ, "{}")), FOXAPI_FAILED));
    // THE NEEDED LIBRARIES: a parameter that is not JSON, an unknown id, a path with no id and a path for a program row are refused
    covering(FOXAPP_OP_NEEDED_LIB_CHECK);
    CHECK(refused(A::apply(a, text(FOXAPP_OP_NEEDED_LIB_CHECK, "not json")), FOXAPI_BAD_ARGUMENT));
    CHECK(refused(A::apply(a, text(FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"no-such-library\"}")), FOXAPI_BAD_ARGUMENT));
    CHECK(refused(A::apply(a, text(FOXAPP_OP_NEEDED_LIB_CHECK, "{\"path\":\"C:/x\"}")), FOXAPI_BAD_ARGUMENT));
    CHECK(refused(A::apply(a, text(FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"sdrplay-sdrconnect\",\"path\":\"C:/x\"}")), FOXAPI_BAD_ARGUMENT));
    covering(FOXAPP_OP_GPU_FFT_MODE);
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_GPU_FFT_MODE, 3)), FOXAPI_OUT_OF_RANGE));
    CHECK(refused(A::apply(a, ints(FOXAPP_OP_GPU_FFT_MODE, 0)), FOXAPI_NO_CHANGE));
}
