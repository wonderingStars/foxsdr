// wire_a_apply_cover.inc - test_apply_command's coverage rule ("every op applyCommand implements has a case") for the ops of wiring lane wire-a. Included inside the
// anonymous namespace of test_apply_command.cpp; each op gets one refusal through the real applyCommand here, the behaviour is held by tests/test_engine_api_wire_a_*.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
void wireAOps(AppWindow& a) {
    covering(FOXAPP_OP_LEVEL_STORE);
    {
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_LEVEL_STORE, 9)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_LEVEL_STORE, 0)), FOXAPI_NO_CHANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_LEVEL_STORE, 2)), FOXAPI_NO_CHANGE));
    }
    covering(FOXAPP_OP_AIRBAND_CHANNEL);
    {
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_AIRBAND_CHANNEL, 9)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_AIRBAND_CHANNEL, 2)), FOXAPI_NO_CHANGE));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_AIRBAND_CHANNEL, "junk")), FOXAPI_BAD_ARGUMENT));
    }
    covering(FOXAPP_OP_PATCH_UNDO);
    covering(FOXAPP_OP_PATCH_REDO);
    covering(FOXAPP_OP_PATCH_SELECT);
    covering(FOXAPP_OP_PATCH_COPY);
    covering(FOXAPP_OP_PATCH_PASTE);
    covering(FOXAPP_OP_PATCH_BYPASS);
    {
        // In the _free variant the engine's own passes have already seen the patch graph the earlier ops of this test changed, so the edit
        // history holds steps (engine merge 4): drain it (bounded by the 100-step history) before the "nothing to undo / redo" refusals.
        for (int i = 0; i < 120 && ok(A::apply(a, ints(FOXAPP_OP_PATCH_UNDO, 0))); ++i) {}
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_PATCH_UNDO, 0)), FOXAPI_NO_CHANGE));
        for (int i = 0; i < 120 && ok(A::apply(a, ints(FOXAPP_OP_PATCH_REDO, 0))); ++i) {}
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_PATCH_REDO, 0)), FOXAPI_NO_CHANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_PATCH_SELECT, 9)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_PATCH_COPY, 0)), FOXAPI_NO_CHANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_PATCH_PASTE, 0)), FOXAPI_NO_CHANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_PATCH_BYPASS, 5)), FOXAPI_OUT_OF_RANGE));
    }
    covering(FOXAPP_OP_RECORD_IQ_CONTAINER);
    {
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_RECORD_IQ_CONTAINER, 3)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_RECORD_IQ_CONTAINER, 0)), FOXAPI_NO_CHANGE));
    }
    covering(FOXAPP_OP_CW_SURROUND);
    {
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_CW_SURROUND, 7)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_CW_SURROUND, "{nope")), FOXAPI_BAD_ARGUMENT));
    }
    covering(FOXAPP_OP_RIT);
    {
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_RIT, 9)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_RIT, 2)), FOXAPI_NO_CHANGE));   // clear already
    }
    covering(FOXAPP_OP_FILE_TRANSPORT);
    {
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_FILE_TRANSPORT, 0)), FOXAPI_UNSUPPORTED));   // the window's radio is on the generator
    }
    covering(FOXAPP_OP_FFT_SCAN);
    {
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_FFT_SCAN, 7)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_FFT_SCAN, 1)), FOXAPI_NO_CHANGE));   // off
    }
    covering(FOXAPP_OP_BAND_MEMORY);
    covering(FOXAPP_OP_BAND_MEMORY_NOTE);
    {
        CHECK(refused(A::apply(a, text(FOXAPP_OP_BAND_MEMORY, "{nope")), FOXAPI_BAD_ARGUMENT));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_BAND_MEMORY_NOTE, "{\"zoomSpanHz\":5}")), FOXAPI_BAD_ARGUMENT));
    }
    covering(FOXAPP_OP_RECORDINGS_SCAN);
    covering(FOXAPP_OP_RECORDINGS_LIST);
    covering(FOXAPP_OP_RECORDINGS_OPEN);
    {
        CHECK(refused(A::apply(a, text(FOXAPP_OP_RECORDINGS_SCAN, "{\"folder\":\"relative\"}")), FOXAPI_BAD_ARGUMENT));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_RECORDINGS_LIST, "{}")), FOXAPI_NO_CHANGE));   // no scan made
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_RECORDINGS_OPEN, 0, 0)), FOXAPI_NO_CHANGE));
    }
    covering(FOXAPP_OP_RECORD_CONTAINER);
    {
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_RECORD_CONTAINER, 5)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_RECORD_CONTAINER, 0)), FOXAPI_NO_CHANGE));   // the default
    }
    covering(FOXAPP_OP_BOOKMARK_FIELDS);
    {
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_BOOKMARK_FIELDS, 987654321)), FOXAPI_NOT_FOUND));
    }
    covering(FOXAPP_OP_TRACE_MODE);
    {
        CHECK(refused(A::apply(a, text(FOXAPP_OP_TRACE_MODE, "sparkly")), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_TRACE_MODE, "normal")), FOXAPI_NO_CHANGE));   // the default
    }
    covering(FOXAPP_OP_DISPLAY_AVERAGE);
    covering(FOXAPP_OP_WF_HISTORY);
    {
        CHECK(refused(A::apply(a, text(FOXAPP_OP_DISPLAY_AVERAGE, "{\"frames\":9999}")), FOXAPI_BAD_ARGUMENT));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_WF_HISTORY, 0)), FOXAPI_NO_CHANGE));   // the history is off
    }
    covering(FOXAPP_OP_OVERLAY_LOAD);
    covering(FOXAPP_OP_OVERLAY_LOOKUP);
    covering(FOXAPP_OP_GRADIENT_SET);
    {
        CHECK(refused(A::apply(a, text(FOXAPP_OP_OVERLAY_LOAD, "{nope")), FOXAPI_BAD_ARGUMENT));
        CHECK(refused(A::apply(a, num(FOXAPP_OP_OVERLAY_LOOKUP, 145.5e6)), FOXAPI_NO_CHANGE));   // nothing loaded
        CHECK(refused(A::apply(a, text(FOXAPP_OP_GRADIENT_SET, "FOXGRAD1|nonsense")), FOXAPI_BAD_ARGUMENT));
    }
    covering(FOXAPP_OP_RECORD_LOCK);
    {
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_RECORD_LOCK, 7)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_RECORD_LOCK, 0)), FOXAPI_NO_CHANGE));   // off already: the default
    }
    covering(FOXAPP_OP_SET_RECORD_DAILY);
    {
        CHECK(refused(A::apply(a, text(FOXAPP_OP_SET_RECORD_DAILY, "{\"style\":9}")), FOXAPI_BAD_ARGUMENT));
    }
    covering(FOXAPP_OP_KEY_ACTION);
    {
        CHECK(refused(A::apply(a, text(FOXAPP_OP_KEY_ACTION, "nonsense")), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_KEY_ACTION, "cycle_step")), FOXAPI_UNSUPPORTED));
    }
    covering(FOXAPP_OP_PROJECT);
    covering(FOXAPP_OP_ATTACH);
    covering(FOXAPP_OP_SIGMF_ANNOTATE);
    {
        CHECK(refused(A::apply(a, text(FOXAPP_OP_PROJECT, "{\"action\":\"nope\"}")), FOXAPI_BAD_ARGUMENT));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_ATTACH, "{\"folder\":\"relative\",\"action\":\"list\"}")), FOXAPI_BAD_ARGUMENT));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_SIGMF_ANNOTATE, "[1]")), FOXAPI_BAD_ARGUMENT));
    }
    covering(FOXAPP_OP_JOB_CANCEL);
    covering(FOXAPP_OP_EXPORT_BOX);
    {
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_JOB_CANCEL, 0)), FOXAPI_NO_CHANGE));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_EXPORT_BOX, "{not json")), FOXAPI_BAD_ARGUMENT));
    }
    covering(FOXAPP_OP_SCAN_PLAN);
    covering(FOXAPP_OP_SCAN_BACK);
    covering(FOXAPP_OP_SCAN_SAVE_HIT);
    covering(FOXAPP_OP_SCAN_TRIGGER);
    covering(FOXAPP_OP_SCANNER_EXTRAS);
    {
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_SCAN_PLAN, 7)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_SCAN_BACK, 0)), FOXAPI_NO_CHANGE));
        CHECK(refused(A::apply(a, num(FOXAPP_OP_SCAN_SAVE_HIT, 145.5e6)), FOXAPI_NO_CHANGE));   // no plan
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_SCAN_TRIGGER, 5)), FOXAPI_OUT_OF_RANGE));
        CHECK(refused(A::apply(a, ints(FOXAPP_OP_SCANNER_EXTRAS, 9)), FOXAPI_OUT_OF_RANGE));
    }
    covering(FOXAPP_OP_PACK_EXPORT);
    covering(FOXAPP_OP_PACK_IMPORT);
    {
        CHECK(refused(A::apply(a, text(FOXAPP_OP_PACK_EXPORT, "{not json")), FOXAPI_BAD_ARGUMENT));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_PACK_IMPORT, "relative.json")), FOXAPI_BAD_ARGUMENT));
    }
    covering(FOXAPP_OP_FAVOURITE_SAVE);
    covering(FOXAPP_OP_FAVOURITE_RECALL);
    covering(FOXAPP_OP_FAVOURITE_EDIT);
    {
        CHECK(refused(A::apply(a, text(FOXAPP_OP_FAVOURITE_SAVE, "{not json")), FOXAPI_BAD_ARGUMENT));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_FAVOURITE_RECALL, "no such favourite")), FOXAPI_NOT_FOUND));
        CHECK(refused(A::apply(a, text(FOXAPP_OP_FAVOURITE_EDIT, "no such favourite")), FOXAPI_NOT_FOUND));
    }
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
    }
}
