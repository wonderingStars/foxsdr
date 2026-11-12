#!/usr/bin/env bash
# Lane sysmodels: one BREAK-IT per behaviour. For each row: apply a one-spot mutation to a source file (exactly once, or the row aborts), rebuild the one test that guards it,
# run it and expect it to FAIL, put the file back with git (new timestamp, so the rebuild is not stale), and move on. A final rebuild + run of every lane test proves GREEN again.
# usage: tools/mutation_batch_sysmodels.sh <worktree> <build-dir> <result-file>
set -u
WT="$1"; BUILD="$2"; OUT="$3"
cd "$WT" || exit 9
: > "$OUT"
EXE="$BUILD/tests/Release"

row() {   # name file target from to
    local name="$1" file="$2" target="$3" from="$4" to="$5"
    if ! python -I tools/sysmodels_mutate.py "$file" "$from" "$to" > "$BUILD/../sysmodels-mut.txt" 2>&1; then
        echo "$name | NOT APPLIED | $(cat "$BUILD/../sysmodels-mut.txt")" >> "$OUT"; git checkout -- "$file"; return
    fi
    if ! cmake --build "$BUILD" --config Release --target "$target" --parallel 3 > "$BUILD/../sysmodels-mut-build.log" 2>&1; then
        echo "$name | BUILD FAILED (mutant did not compile) |" >> "$OUT"; git checkout -- "$file"; return
    fi
    "$EXE/$target.exe" > "$BUILD/../sysmodels-mut-run.log" 2>&1
    local rc=$?
    local fails; fails=$(grep -c '^FAIL' "$BUILD/../sysmodels-mut-run.log")
    local last; last=$(tail -1 "$BUILD/../sysmodels-mut-run.log")
    if [ "$rc" -ne 0 ]; then echo "$name | RED (exit $rc, $fails FAIL lines) | $last" >> "$OUT"; else echo "$name | SURVIVED (exit 0) | $last" >> "$OUT"; fi
    git checkout -- "$file"
}

row "sha-256 round function"          src/core/sys_hash.cpp           test_sys_ed25519 'rotr32(e, 6) ^ rotr32(e, 11)' 'rotr32(e, 7) ^ rotr32(e, 11)'
row "ed25519 S below L rule"           src/core/sys_ed25519.cpp        test_sys_ed25519 'if (!sLessThanL(sig.data() + 32)) { return false; }' ''
row "ed25519 canonical y rule"         src/core/sys_ed25519.cpp        test_sys_ed25519 'if (!yBelowP(p)) { return false; }' ''
row "ed25519 x=0 sign-bit rule"        src/core/sys_ed25519.cpp        test_sys_ed25519 'if (!neq25519(r[0], kGf0) && sign == 1) { return false; }' ''
row "manifest sig: no trusted key"     src/core/sys_manifest_sig.cpp   test_sys_ed25519 'if (trusted.empty()) { return SigResult::NoTrustedKey; }' ''
row "manifest sig: payload bound"      src/core/sys_manifest_sig.cpp   test_sys_ed25519 'std::string signedBytes(const std::string& payload) { return std::string("FoxSDR update manifest v1\n") + payload; }' 'std::string signedBytes(const std::string& payload) { return payload; }'
row "manifest tool: verify verdict"    tools/manifest_sign.cpp         test_sys_manifest_tool 'if (r != SigResult::Ok) { std::fprintf(stderr, "manifest_sign: not valid: %s\n", sigResultName(r)); return 2; }' ''
row "dormant signing script refusal"   installer/sign-setup.ps1        test_sys_manifest_tool 'no certificate is named (set FOXSDR_SIGN_CERT_SHA1 or FOXSDR_SIGN_PFX)' 'proceeding'
row "keep-awake: clear on release"     src/core/sys_keep_awake.cpp     test_sys_power 'if (fns_.setState) { fns_.setState(kEsContinuous); }' ''
row "resume: rate limit"               src/core/sys_resume_watch.cpp   test_sys_power 'if (everFired_ && at < lastFire_ + cfg_.minIntervalMs) { at = lastFire_ + cfg_.minIntervalMs; }' ''
row "resume: suspend cancels"          src/core/sys_resume_watch.cpp   test_sys_power 'pending_ = false;
            suspended_ = true;' 'suspended_ = true;'
row "device watch: radio-class filter" src/core/sys_device_watch.cpp   test_sys_power 'if (!info.valid || !info.isUsbRadioClass) { return false; }' 'if (!info.valid) { return false; }'
row "resource meter: bad GPU values"   src/core/sys_resource_meter.cpp test_sys_resources 'if (!(inst.value >= 0.0) || inst.value > 1.0e6) { continue; }' ''
row "effects policy: hysteresis"       src/core/sys_effects_policy.hpp test_sys_resources 'if (!reduced_ && highRun_ >= t_.enterSamples)' 'if (!reduced_ && highRun_ >= 1)'
row "portable: containment (links)"    src/core/sys_portable.cpp       test_sys_portable 'if (ec1 || ec2) { return false; }' 'return true;'
row "bundle: sha256 check"             src/core/sys_bundle.cpp         test_sys_bundle 'if (sha256Hex(bytes) != dl.sha) { return fail(BundleError::HashMismatch, dl.name); }' ''
row "bundle: listeners imported off"   src/core/sys_bundle.cpp         test_sys_bundle '*it = false;' ''
row "bundle: confirm token"            src/core/sys_bundle.cpp         test_sys_bundle 'if (token.empty() || token != plan.confirmToken) {' 'if (false) {'
row "bundle: allow-list of names"      src/core/sys_bundle.cpp         test_sys_bundle 'bool allowedBundleFileName(const std::string& name) { return name == "config.json" || name == "bookmarks.json" || name == "markers.json"; }' 'bool allowedBundleFileName(const std::string& name) { return !name.empty(); }'
row "reset: confirm token"             src/core/sys_reset.cpp          test_sys_setup 'if (token.empty() || token != plan.confirmToken) { error = "the confirmation does not match what is in the folders now"; return false; }' ''
row "reset: folder-name guard"         src/core/sys_reset.cpp          test_sys_setup 'if (settingsDir.empty() || !namedFoxsdr(settingsDir)) { plan.error = "the settings folder is not a folder named foxsdr; refusing"; return plan; }' ''
row "profiles: machine keys carried"   src/core/sys_profiles.cpp       test_sys_setup 'fresh[it.key()] = it.value(); sp.carriedKeys.push_back(it.key());' ''
row "profiles: sync conflict"          src/core/sys_profiles.cpp       test_sys_setup 'else { item.action = SyncAction::Conflict; }' 'else { item.action = SyncAction::PushToShared; }'
row "first run: preset fit rule"       src/core/sys_first_run.cpp      test_sys_setup 'if (b.freqHz >= w.first && b.freqHz <= w.second) { inside = true; }' 'inside = true;'
row "telemetry: off clears pending"    src/core/sys_telemetry_switch.hpp test_sys_setup 'o.next.pending.clear();' ''
row "telemetry: no send when off"      src/core/sys_telemetry_switch.hpp test_sys_setup 'inline bool mayTransmit(const TelemetryState& s) { return s.enabled && wellFormedInstallId(s.installId); }' 'inline bool mayTransmit(const TelemetryState& s) { return wellFormedInstallId(s.installId); }'

# GREEN AGAIN: everything restored, rebuild all lane tests, run them.
cmake --build "$BUILD" --config Release --target test_sys_ed25519 test_sys_manifest_tool test_sys_power test_sys_resources test_sys_portable test_sys_bundle test_sys_setup --parallel 3 > "$BUILD/../sysmodels-mut-final-build.log" 2>&1
echo "final rebuild exit $?" >> "$OUT"
for t in test_sys_ed25519 test_sys_manifest_tool test_sys_power test_sys_resources test_sys_portable test_sys_bundle test_sys_setup; do
    "$EXE/$t.exe" > "$BUILD/../sysmodels-mut-final-$t.log" 2>&1
    echo "restored $t | exit $? | $(tail -1 "$BUILD/../sysmodels-mut-final-$t.log")" >> "$OUT"
done
echo DONE >> "$OUT"
