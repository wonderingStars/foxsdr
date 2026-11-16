// fixture for tests/errpath_gate_selftest.cpp: scanned as TEXT, never compiled.
void closeIt(void* s) {
    Pa_CloseStream(s);
    (void)Pa_AbortStream(s);
    api_->hackrf_close(dev);
    if (x) rtlsdr_close(dev);
    const int ok = Pa_StopStream(s);
    (void)ok;
}
