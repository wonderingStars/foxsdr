// test_soapy_model_report.cpp - A RADIO OPENED THROUGH SOAPYSDR IS REPORTED BY ITS OWN MODEL, THE VENDOR DRIVER KEY IN ITS OWN FIELD (hardware check 2026-11-17, docs/multi-radio-records/hw-findings.md item 5).
//
// The check found every SoapySDR radio would appear as model "uhd" on the owner's radios page. Here the queued messages themselves are read (a stand-in server, nothing is sent):
//   A  a failed open (radio_fail) and a journal event (radio_settings) of a B200 carry model "B200", serial unchanged, key b200:EDR04ZDB2, and `library` "SoapySDR uhd";
//   B  a hand-written row that names only the driver and the serial is "uhd" until the model is learned (a scan row, or the open's hardware key) and then "B200" with the same key;
//   C  a native radio is untouched: no `library`, model from its label;
//   D  nothing but the model, the key and the library changed: the serial is the radio's own and no address or label text reaches either field.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>
#include <vector>

#include "radio_report_rig.hpp"
#include "test_check.hpp"

int main() {
    std::printf("test_soapy_model_report\n");
    namespace rr = cascade::core::radioreport;
    const std::string full = "driver=uhd, label=B200 EDR04ZDB2, name=, product=B200, serial=EDR04ZDB2, type=b200";

    {   // A: a failed open and a refused setting of the B200
        rrrig::Rig t;
        rr::FailureInput in;
        in.driverKind = "soapy"; in.args = full; in.reason = "the radio is busy: in use by another program"; in.reasonClass = "busy";
        in.trigger = rr::Trigger::UserPick;
        rr::global().fileFailure(in, {});
        int dummy = 0;
        const rr::RadioIdentity id = rr::identify("soapy", full);
        rr::registerSource(&dummy, id);
        rr::noteControlRefused(&dummy, "soapy", rr::Control::Gain, "the driver refused that gain");
        const std::vector<rrrig::json> all = t.flush();
        const std::vector<rrrig::json> fails = rrrig::ofKind(all, "radio_fail");
        const std::vector<rrrig::json> sets = rrrig::ofKind(all, "radio_settings");
        CHECK(fails.size() == 1 && sets.size() == 1);
        for (const std::vector<rrrig::json>* v : {&fails, &sets}) {
            for (const rrrig::json& m : *v) {
                const auto& b = m["body"];
                CHECK(b["driver"] == "soapy" && b["model"] == "B200" && b["serial"] == "EDR04ZDB2" && b["library"] == "SoapySDR uhd");
                CHECK(m["radioKey"] == "b200:EDR04ZDB2");
                const std::string dump = m.dump();
                CHECK(dump.find("\"uhd\"") == std::string::npos);   // the driver key is only ever inside `library`
            }
        }
        rr::forgetRadio(id.key);
    }

    {   // B: the minimal saved row
        rrrig::Rig t;
        const std::string minimal = "driver=uhd,serial=EDR04ZDB9";
        rr::FailureInput in;
        in.driverKind = "soapy"; in.args = minimal; in.reason = "no match"; in.reasonClass = "absent"; in.trigger = rr::Trigger::StartupRestore;
        rr::global().fileFailure(in, {});
        const std::vector<rrrig::json> first = rrrig::ofKind(t.flush(), "radio_fail");
        CHECK(first.size() == 1 && first[0]["body"]["model"] == "uhd" && first[0]["body"]["library"] == "SoapySDR uhd");   // nothing knows better yet
        rr::rememberSoapyModel(minimal, "B200");   // the open reported its hardware key
        in.reason = "the radio is busy: in use by another program"; in.reasonClass = "busy";   // a different reason class so the daily bound sends it
        rr::global().fileFailure(in, {});
        const std::vector<rrrig::json> second = rrrig::ofKind(t.flush(), "radio_fail");
        CHECK(second.size() == 1 && second[0]["body"]["model"] == "B200" && second[0]["radioKey"] == "b200:EDR04ZDB9");
    }

    {   // C: a native radio: no library, the model from its label
        rrrig::Rig t;
        rr::FailureInput in;
        in.driverKind = "rtlsdr"; in.args = "serial=00000001"; in.nativeLabel = "RTL2838UHIDIR (serial 00000001)";
        in.reason = "the radio is in use by another program"; in.reasonClass = "busy"; in.trigger = rr::Trigger::UserPick;
        rr::global().fileFailure(in, {});
        const std::vector<rrrig::json> fails = rrrig::ofKind(t.flush(), "radio_fail");
        CHECK(fails.size() == 1);
        if (fails.size() == 1) {
            CHECK(fails[0]["body"]["model"] == "RTL2838UHIDIR" && !fails[0]["body"].contains("library"));
        }
    }

    return testSummary("test_soapy_model_report");
}
