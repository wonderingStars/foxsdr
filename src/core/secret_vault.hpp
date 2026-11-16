// secret_vault.hpp - the settings file's secrets live in the Windows per-user protected store, not in the file (owner decision 16, 2026-11-16).
//
// WHAT IS A SECRET HERE. The top-level keys of a settings document named by secretKeys(): the tester's two tokens and the web page's password
// record. (The e-mail alert password, the InfluxDB token and the MQTT password are options of blocks that no setting reaches yet: when one does, its
// key is added to secretKeys() and nothing else changes.)
//
// WHICH STORE, AND WHY. DPAPI (CryptProtectData, current-user scope, CRYPTPROTECT_UI_FORBIDDEN so it can never raise a prompt), not the Credential
// Manager. The tree used neither before. DPAPI keeps the protected bytes in a file we choose, so a test writes into a scratch folder and never
// touches the real user's credential list (the Credential Manager is one per-user list shared by every folder, and a test could not be kept away
// from it); the blob can only be opened by the same Windows user on the same machine, which is the property the owner asked for; and it needs no
// extra library (crypt32 is already linked for the web page's HTTPS). Each item carries its own key name as DPAPI "optional entropy", so a blob
// copied under another key name does not open.
//
// THE SIDECAR. "<settings file>.secrets" beside the settings file: {"format":"foxsdr-secrets","version":1,"items":{"<key>":"<hex of the blob>"}}.
// The settings file keeps the key with an EMPTY value. load() puts the values back before the document is read, so every reader sees the settings
// file as it always was; save()'s writer takes them out. A settings file that still holds a value in plain text (one written by an older build) is
// MIGRATED on its first load: the value goes to the sidecar, the file is rewritten without it, and the dated ".bak" copies of the file (made by the
// writer before every replace) are scrubbed of it too.
//
// LINUX AND ANYWHERE WITHOUT DPAPI. There is no per-user protected store there. The sidecar then holds the value as hex text under "plain:" and
// its permissions are set to owner read and write only (0600); the settings file's own copy is still removed. The proper Linux store (the Secret
// Service or a kernel keyring) is an open row of the E1 lane's record.
//
// A SECRET THAT CANNOT BE OPENED (another Windows user, another machine, a damaged sidecar) reads as EMPTY and is reported in `note`: the person
// signs in or pastes it again. Nothing is ever deleted because it could not be read.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>
#include <vector>

namespace cascade::core::vault {

// The top-level settings keys whose values are kept in the protected store.
const std::vector<std::string>& secretKeys();

// A PLUGIN'S SECRET SETTINGS. The settings file keeps plugin fields as "pluginSettings": {"<plugin>": {"<key>": "<value>"}}. A field is a secret when
// the plugin declares it a PASSWORD field (the host calls declarePluginSecret when it loads the plugin: the e-mail alert password and the
// InfluxDB token of the alert and logging plugins are such fields), or when its key names one (password, passwd, passphrase, secret, token,
// apikey, api_key, credential, privatekey) - so a value written by an older build is found on the first load, before any plugin is loaded.
// The protected-store item is named "pluginSettings/<plugin>/<key>".
void declarePluginSecret(const std::string& plugin, const std::string& key);
bool isPluginSecret(const std::string& plugin, const std::string& key);
void forgetDeclaredPluginSecretsForTest();

// "<path>.secrets".
std::string sidecarPath(const std::string& settingsPath);

// True when this build has a per-user protected store (Windows). False: the sidecar holds the 0600 plain form.
bool hasProtectedStore();

// Protect and unprotect one value (hex out and in). `key` is the entropy. False with `why` set; never raises a window.
bool protectValue(const std::string& key, const std::string& plain, std::string& hexOut, std::string& why);
bool unprotectValue(const std::string& key, const std::string& hexIn, std::string& plainOut, std::string& why);

// THE WRITER'S HALF. `text` is a settings document about to be written to `settingsPath`. Every secret key with a non-empty value is moved into
// the sidecar and emptied in the returned text; a secret key present and empty removes that item (the person cleared it). Text that is not a JSON
// object, or holds no secret key, is returned as it came. On a failure to store (`why` set, `kept` true) the value is LEFT in the returned text
// so that nothing is lost: the caller reports it.
std::string splitForWrite(const std::string& settingsPath, const std::string& text, std::string& why, bool& kept);

// THE READER'S HALF. `text` is what the settings file holds. Empty secret keys are filled from the sidecar. A secret that is still in the file as
// text is MIGRATED (stored, the file rewritten without it, the ".bak" copies scrubbed): `migrated` is set. `note` says what could not be opened.
// The returned text is what the settings parser should read.
std::string mergeAfterRead(const std::string& settingsPath, const std::string& text, bool& migrated, std::string& note);

// Removes the sidecar (a settings file that was replaced wholesale, a reset).
void forget(const std::string& settingsPath);

// Removes every secret key's value from every dated ".bak" copy of `settingsPath` (they are whole copies of older files).
int scrubBackups(const std::string& settingsPath);

}  // namespace cascade::core::vault
