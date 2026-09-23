// Tests for gui/input_script.hpp - the scripted pointer for bounded runs.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/input_script.hpp"

#include "test_check.hpp"

using cascade::gui::parseInputScript;
using cascade::gui::ScriptParse;
using cascade::gui::ScriptStep;

int main() {
    // [1] Every verb, in order, with comments and blank lines ignored.
    {
        const ScriptParse p = parseInputScript(
            "# a comment line\n"
            "\n"
            "100 world 250.5 -40\n"
            "101 down   # trailing comment\n"
            "102 up\r\n"
            "110 screen 800 600\n"
            "120 key ctrl+a\n"
            "121 text 100.310 MHz\n"
            "122 key enter\n");
        CHECK(p.bad == 0);
        CHECK(p.steps.size() == 7u);
        if (p.steps.size() == 7u) {
            CHECK(p.steps[0].verb == ScriptStep::Verb::World);
            CHECK(p.steps[0].frame == 100);
            CHECK(p.steps[0].x == 250.5f);
            CHECK(p.steps[0].y == -40.0f);
            CHECK(p.steps[1].verb == ScriptStep::Verb::Down);
            CHECK(p.steps[2].verb == ScriptStep::Verb::Up);   // CRLF tolerated
            CHECK(p.steps[3].verb == ScriptStep::Verb::Screen);
            CHECK(p.steps[4].verb == ScriptStep::Verb::Key);
            CHECK(p.steps[4].arg == "ctrl+a");
            CHECK(p.steps[5].verb == ScriptStep::Verb::Text);
            CHECK(p.steps[5].arg == "100.310 MHz");            // spaces kept
            CHECK(p.steps[6].arg == "enter");
        }
    }

    // [2] Anything malformed is COUNTED, not silently dropped - a step that
    // cannot run is a test that checks less than it says.
    {
        const ScriptParse p = parseInputScript(
            "10 world 5\n"          // missing y
            "11 wiggle\n"           // unknown verb
            "12 key hyperspace\n"   // unknown key
            "13 text\n"             // nothing to type
            "abc down\n"            // no frame
            "20 down\n"             // fine
            "15 up\n"               // goes backwards
            "-1 up\n");             // negative
        CHECK(p.bad == 7);
        CHECK(p.steps.size() == 1u);
        CHECK(!p.steps.empty() && p.steps[0].frame == 20);
    }

    // [3] Two steps in one frame keep their order.
    {
        const ScriptParse p = parseInputScript("5 down\n5 up\n");
        CHECK(p.bad == 0);
        CHECK(p.steps.size() == 2u);
        CHECK(p.steps.size() == 2u && p.steps[0].verb == ScriptStep::Verb::Down);
    }

    // [4] The wheel: notches as given, and a wheel that moves nothing is a
    // mistake, not a step.
    {
        const ScriptParse p = parseInputScript("5 wheel -3\n6 wheel 1.5\n7 wheel 0\n8 wheel\n");
        CHECK(p.bad == 2);
        CHECK(p.steps.size() == 2u);
        CHECK(p.steps.size() == 2u && p.steps[0].verb == ScriptStep::Verb::Wheel && p.steps[0].y == -3.0f);
        CHECK(p.steps.size() == 2u && p.steps[1].y == 1.5f);
    }

    return testSummary("test_input_script");
}
