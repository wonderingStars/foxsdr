# Field fixes g (branch fix/field-g, cut from 03ac57b7, 2026-11-19; local only, nothing pushed)

Source: D:/foxsdr-build/prep/FIELD-RADIOS-2026-11-19.md. Website half: private site repo, branch fix/field-g, PART 14 of HANDOVER-daylight.md.

## Final state (all of A1 to A5 and the application half of W2 done; local commits only)

- A1: 2.0 MS/s added to the RTL-SDR menu (rtl2832u.cpp supportedRatesHz). Cause: config.hpp:508 default 2000000 Hz had no menu entry; RtlSdrSource::setSampleRateHz snapped it and rate_follow_status.hpp (applySourceRate) journalled the snap. RED seen then GREEN (test_rtlsdr_source 262/0). Unverified on a real dongle.
- A2: engine_radio_reports.cpp journalStreamMinute returns for a faulted slot (the slot stays open after the driver already reported the close). Test: the netfault block of test_radio_reports_engine. RED not separately observed.
- A3: contract 3.4 already allows it. radioreport::noteRateApplied (radio_events.cpp) called from applySourceRate when no `why` is given (a person or a plugin preset); open and restore are said by the open's own settings. No new field. Test in test_radio_events (messages pass the Worker validator).
- A4: Reporter::configure (radio_reporter.cpp) filed held failures before held events; both now carry an arrival number and are filed merged by it. New test in test_radio_reporter. RED not separately observed.
- A5: pathsToToken (radio_report.cpp) continues a drive path over a space in a folder name (rule in the code comment). Tests in test_radio_report (Program Files, (x86), profile "John Smith", two non-swallow cases). RED not separately observed.
- W2 (application side, decided here not on the site): radioReportsScanLanded skips a radio_found whose serial another route or driver already listed (native list kept across the separately landing scans). Test phantomFoundTest in test_radio_reports_engine, RED seen (stock=2) then GREEN. Cost: two different radios of different drivers sharing one stock serial are said once per scan.
- NOTE: main() in test_radio_reports_engine.cpp is near the stack limit; one more block of locals made the exe die with a stack overflow before any output, so new blocks go in their own function.
- No contract fixture or field changed; the Worker is untouched (test_radio_events validates the new radio_settings events with the Worker's validator: 0 refused).
- A6: nothing touched of "one failure report per retry" or the second copy.
- Suites run green: test_radio_report 222, test_radio_reporter 230, test_radio_reports_never_sent 9, test_radio_reports_engine 150, test_radio_reports_fields 48, test_radio_events 116, test_upload_scrub 316, test_rate_follow 533, test_rtlsdr_direct 45, test_pipeline_device_rate_audio 315, test_rtlsdr_source 262, test_error_paths 35, errpath_gate_selftest 42. Not run: the Node contract fixture validator (tests/radio_reports_validate.mjs) and the Worker's own tests.
