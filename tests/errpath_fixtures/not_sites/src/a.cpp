// fixture for tests/errpath_gate_selftest.cpp: scanned as TEXT, never compiled.
// diagWarnf("in a comment") lastError_ = "in a comment";
void quiet() {
    const char* s = "diagWarnf(\"in a string\"); setError(\"x\");";
    const char* r = R"x(diagWarnf("in a raw string"); catch (...) {})x";
    diagLogf("started %d receivers", 3);
    int error = 0;
    std::string lastError_;
    lastError_ = "";
    lastError_ = {};
    if (lastError_ == "x") {}
    try { run(); } catch (const std::exception& e) { use(e); }
    try { run(); } catch (...) { diagLogf("kept going"); throw; }
    int e2 = Pa_CloseStream(s);
    (void)s; (void)r; (void)e2; (void)error;
}
struct Holder { std::string error = "none"; };
enum class Code { kBadError = 3 };
