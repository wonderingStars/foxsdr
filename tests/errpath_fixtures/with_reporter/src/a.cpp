// fixture for tests/errpath_gate_selftest.cpp: scanned as TEXT, never compiled.
void openIt(int id) {
    if (id < 0) {
        diagWarnf("radio %d would not open", id);
        health::noteRecovered(Recovered::OpenFailed);
    }
}
