// fixture for tests/errpath_gate_selftest.cpp: scanned as TEXT, never compiled.
void openIt(int id) {
    if (id < 0) {
        diagWarnf("radio %d would not open", id);
    }
}
void outer(int id) {
    if (id < 0) {
        diagWarnf("outer failed");
        health::noteRecovered(Recovered::OpenFailed);
    }
}
