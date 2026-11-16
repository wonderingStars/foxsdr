// fixture for tests/errpath_gate_selftest.cpp: scanned as TEXT, never compiled.
namespace cascade::source {
class Foo {
public:
    void inlineFail() { diagWarnf("inline failed"); }
    Foo() : a_{1}, b_(2) { diagWarnf("ctor failed"); }
    void outOfLine();
private:
    int a_, b_;
};
void Foo::outOfLine() { diagWarnf("outer failed"); }
}  // namespace cascade::source
