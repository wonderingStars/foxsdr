// extend_publish.cpp - the EXTEND bank's publisher and the Plugin store page's (extend_publish.hpp): the host's models (extend_logic.hpp) as the window's generated
// structs, the window's gestures as the Effects extend_logic.hpp names for them. SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "extend_publish.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>

#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "core/disk_job.hpp"
#include "core/i18n.hpp"
#include "core/plugin_store_reasons.hpp"
#include "core/png_decode.hpp"

namespace foxsdr::slintextend {

namespace cc = cascade::core;
using cascade::i18n::tr;

namespace {

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

::ChipTone tone(Tone t) {
    switch (t) {
        case Tone::Good: return ::ChipTone::Good;
        case Tone::Warn: return ::ChipTone::Warn;
        case Tone::Err: return ::ChipTone::Err;
        default: return ::ChipTone::Neutral;
    }
}

std::vector<::SelectItem> items(const std::vector<SelectItem>& v) {
    std::vector<::SelectItem> out;
    out.reserve(v.size());
    for (const SelectItem& s : v) {
        ::SelectItem o{};
        o.label = ss(s.label);
        o.id = ss(s.id);
        o.enabled = s.enabled;
        o.note = ss(s.note);
        out.push_back(std::move(o));
    }
    return out;
}

std::vector<slint::SharedString> strings(const std::vector<std::string>& v) {
    std::vector<slint::SharedString> out;
    out.reserve(v.size());
    for (const std::string& s : v) { out.push_back(ss(s)); }
    return out;
}

template <class T> std::shared_ptr<slint::Model<T>> model(std::vector<T> v) { return std::make_shared<slint::VectorModel<T>>(std::move(v)); }

std::vector<::StoreBadge> badges(const std::vector<StoreBadge>& v) {
    std::vector<::StoreBadge> out;
    for (const StoreBadge& b : v) {
        ::StoreBadge o{};
        o.text = ss(b.text);
        o.tone = tone(b.tone);
        out.push_back(std::move(o));
    }
    return out;
}

void sigAdd(std::string& sig, const std::string& s) {
    sig += s;
    sig += '\x1f';
}
void sigAdd(std::string& sig, long long v) {
    sig += std::to_string(v);
    sig += '\x1f';
}
void sigBadges(std::string& sig, const std::vector<StoreBadge>& v) {
    for (const StoreBadge& b : v) {
        sigAdd(sig, b.text);
        sigAdd(sig, static_cast<int>(b.tone));
    }
    sig += '\x1e';
}

// The glyph tile of the Slint card takes a letter or a symbol, where the ImGui window draws a vector picture of the category: the plugin's first letter stands for it (UTF-8
// aware, ASCII upper-cased), the category id's when the plugin has no name.
std::string glyphOf(const std::string& name, const std::string& category) {
    const std::string& from = name.empty() ? category : name;
    if (from.empty()) { return std::string(); }
    const unsigned char lead = static_cast<unsigned char>(from[0]);
    const std::size_t len = lead < 0x80u ? 1u : lead >= 0xF0u ? 4u : lead >= 0xE0u ? 3u : lead >= 0xC0u ? 2u : 1u;
    std::string out = from.substr(0, std::min(len, from.size()));
    if (out.size() == 1u) { out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0]))); }
    return out;
}

std::string readFile(const std::string& path) {
    std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    if (!in.good()) { return std::string(); }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

constexpr double kAskExpiryS = 6.0;          // a SERVER_CONFIG's answer not seen by then is given up on (the kit's poll can take it first)
constexpr std::int64_t kStoreBusyEveryMs = 250;   // the transfer's figure moves with no list moving
constexpr std::int64_t kStoreEveryMs = 5000;      // insurance: the catalogue line's "from <date>" and anything that moved without listSeq
constexpr int kCaptureTicks = 240;                // FOXSDR_SLINT_STORE_PAGE waits this long for the catalogue to list the id

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// A list model kept between publishes and updated row by row (the pattern of system_publish.cpp).
template <class T>
struct Synced {
    std::shared_ptr<slint::VectorModel<T>> model = std::make_shared<slint::VectorModel<T>>();
    std::shared_ptr<slint::Model<T>> set(const std::vector<T>& rows) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (i < model->row_count()) {
                if (!(*model->row_data(i) == rows[i])) { model->set_row_data(i, rows[i]); }
            } else {
                model->push_back(rows[i]);
            }
        }
        while (model->row_count() > rows.size()) { model->erase(model->row_count() - 1u); }
        return model;
    }
};

struct ExtendPublisher::Lists {
    Synced<::SelectItem> reach;
    Synced<slint::SharedString> addresses;
};

// The store's screenshots: a picture file decoded on a worker, one at a time (the ImGui window decodes one a frame on its own thread; this one decodes none there), and
// kept by the file's sha256 - the catalogue's own key for it.
struct ExtendPublisher::Pictures {
    struct Result {
        std::string sha, error;
        int width = 0, height = 0;
        std::vector<unsigned char> rgba;
    };
    struct Entry {
        slint::Image image;
        std::string error;   // English, as the decoder said it: translated where it is drawn
    };
    cc::DiskJob<Result> job{"Store picture decode"};
    std::string inFlight;
    std::map<std::string, Entry> done;
    std::uint64_t revision = 0;
};

// An answer this object waits for: a SERVER_CONFIG's, kept with the clock it was sent at.
struct ExtendPublisher::Pending {
    struct Ask {
        std::uint64_t ticket = 0;
        double at = 0.0;
    };
    std::vector<Ask> asks;
    std::set<std::uint32_t> withheldLogged;   // the ops a bounded run said it would not send (once each)
};

struct ExtendPublisher::Last {
    std::optional<::WebModel> web;
    std::optional<::CatModel> cat;
    std::string storeSig;
    bool haveStoreSet = false;
};

ExtendPublisher::ExtendPublisher(MainWindow& ui, foxsdr::slintkit::PluginKit& kit, foxsdr::slintkit::KitPublisher& pages, const FoxEngineApi* api, FoxSession* session, bool bounded)
    : ui_(ui), kit_(kit), pages_(pages), api_(api), session_(session), bounded_(bounded), table_(api, session), lists_(std::make_unique<Lists>()),
      pictures_(std::make_unique<Pictures>()), pending_(std::make_unique<Pending>()), lastSet_(std::make_unique<Last>()) {}

ExtendPublisher::~ExtendPublisher() = default;

double ExtendPublisher::nowS() const { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void ExtendPublisher::say(const std::string& line) const { std::fprintf(stderr, "foxsdr-slint: extend: %s\n", line.c_str()); }

// ---------------------------------------------------------------------------------------------------------------------------------
// commands and effects

// One command through the kit's submit (a long text goes by its long-text channel); the ticket, 0 when the session would not take it. A bounded run keeps it: the op is
// said once, the text never.
std::uint64_t ExtendPublisher::send(const Command& c) {
    forceRead_ = true;
    if (!c.valid) { return 0u; }
    if (bounded_) {
        ++withheld_;
        if (pending_->withheldLogged.insert(c.q.c.op).second) {
            cc::diagLogf("extend: bounded run - op 0x%04x not sent", static_cast<unsigned>(c.q.c.op));
            std::fprintf(stderr, "foxsdr-slint: extend: bounded run - op 0x%04x not sent\n", static_cast<unsigned>(c.q.c.op));
        }
        return 0u;
    }
    const std::uint64_t ticket = kit_.submit(c.q);
    if (ticket != 0u) { ++sent_; }
    return ticket;
}

void ExtendPublisher::applyLocal(const Effect& e) {
    switch (e.local) {
        case Local::None: break;
        case Local::Web:
            opts_.web = e.web;
            last_.web.draft = e.web;   // the next gesture of the same tick starts from this one
            break;
        case Local::Cat:
            opts_.cat = e.cat;
            last_.cat.draft = e.cat;
            break;
        case Local::Store:
            opts_.store = e.store;
            last_.store.draft = e.store;
            storeForce_ = true;
            break;
    }
}

// What the window does with an Effect: the commands in order (an ask kept against the ticket of each), then the draft it carries; and a password does not outlive the send.
void ExtendPublisher::exec(Effect e, const char* what) {
    (void)what;
    forceRead_ = true;
    if (e.empty()) { return; }
    for (const Command& c : e.commands) {
        const std::uint64_t ticket = send(c);
        if (ticket != 0u && e.ask.kind == AskKind::ServerConfig) {
            pending_->asks.push_back({ticket, nowS()});
            ++opts_.server_asks_pending;
        }
    }
    if (e.local != Local::None) { applyLocal(e); }
    if (e.pictures_asked) { picturesAskedSeq_ = listSeq_; }
    // The send is done: the effect's own copies of the password go (the draft the window keeps was copied by applyLocal above).
    scrubEffect(e);
}

// An answer polled by main.cpp: a SERVER_CONFIG's. The engine's refusal becomes the web draft's error line (applyServerAnswer).
void ExtendPublisher::onAnswer(const FoxCommandResult& r) {
    auto& asks = pending_->asks;
    const auto it = std::find_if(asks.begin(), asks.end(), [&](const Pending::Ask& a) { return a.ticket == r.ticket; });
    if (it == asks.end()) { return; }
    asks.erase(it);
    if (opts_.server_asks_pending > 0) { --opts_.server_asks_pending; }
    applyServerAnswer(opts_.web, r.status, r.message);
    applyServerAnswer(last_.web.draft, r.status, r.message);
    forceRead_ = true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the pictures

void ExtendPublisher::pollPictures() {
    Pictures& p = *pictures_;
    Pictures::Result r;
    if (!p.job.poll(r)) { return; }
    if (p.job.threw()) {   // the worker threw: its Result is blank; the picture is the one that was out
        r = Pictures::Result{};
        r.sha = p.inFlight;
        r.error = "the picture file cannot be read";
    }
    p.inFlight.clear();
    if (r.sha.empty()) { return; }
    Pictures::Entry e;
    if (!r.error.empty()) {
        e.error = r.error;
    } else if (r.width > 0 && r.height > 0 && r.rgba.size() >= static_cast<std::size_t>(r.width) * static_cast<std::size_t>(r.height) * 4u) {
        slint::SharedPixelBuffer<slint::Rgba8Pixel> buf(static_cast<std::uint32_t>(r.width), static_cast<std::uint32_t>(r.height));
        std::memcpy(buf.begin(), r.rgba.data(), static_cast<std::size_t>(r.width) * static_cast<std::size_t>(r.height) * 4u);
        e.image = slint::Image(buf);
    } else {
        e.error = "the picture file cannot be read";
    }
    p.done[r.sha] = std::move(e);
    ++p.revision;
    storeForce_ = true;
}

// A picture the page lists as ready and nobody has decoded: one worker at a time, the file read and decoded off this thread (decodePng refuses a header that claims more than
// 4096 a side before it allocates a pixel).
void ExtendPublisher::wantPictures(const StorePluginPage& page) {
    Pictures& p = *pictures_;
    if (p.job.pending()) { return; }
    for (const PictureFrame& f : page.screenshots) {
        if (f.sha256.empty() || f.state != kPicReady || f.path.empty() || p.done.count(f.sha256) > 0) { continue; }
        const std::string sha = f.sha256, path = f.path;
        const bool started = p.job.request([sha, path]() {
            Pictures::Result r;
            r.sha = sha;
            const std::string bytes = readFile(path);
            if (bytes.empty()) {
                r.error = "the picture file cannot be read";
                return r;
            }
            const std::vector<unsigned char> raw(bytes.begin(), bytes.end());
            cc::PngImage img;
            std::string err;
            if (!cc::decodePng(raw, 4096, img, err)) {
                r.error = err.empty() ? std::string("the picture file cannot be read") : err;
                return r;
            }
            r.width = img.width;
            r.height = img.height;
            r.rgba = std::move(img.rgba);
            return r;
        });
        if (started) { p.inFlight = sha; }
        return;
    }
}

// The screenshots of the open page as images: one a frame the catalogue lists, blank while a frame has no picture (loading, failed, undecodable). While NO frame has one the
// list is empty and the words are the first frame's (the well shows them): "No pictures published yet", "loading...", the failure's reason. With some, the note is the current
// frame's caption, and its words too when it has no picture.
std::shared_ptr<slint::Model<slint::Image>> ExtendPublisher::mapScreenshots(const StorePluginPage& page, std::string& note) {
    std::vector<slint::Image> images;
    std::vector<std::string> words;
    std::vector<std::string> captions;
    bool any = false;
    for (const PictureFrame& f : page.screenshots) {
        if (f.sha256.empty()) { continue; }
        slint::Image image;
        std::string word = f.word;
        if (f.state == kPicReady) {
            const auto it = pictures_->done.find(f.sha256);
            if (it == pictures_->done.end()) {
                word = tr("loading...");
            } else if (!it->second.error.empty()) {
                word = cascade::gui::trStoredReason(it->second.error);
            } else {
                image = it->second.image;
                word.clear();
                any = true;
            }
        }
        images.push_back(std::move(image));
        words.push_back(std::move(word));
        captions.push_back(f.caption);
    }
    if (images.empty()) {
        // no frame carries a sha: the lone "No pictures published yet" frame (or none at all)
        note = page.screenshots.empty() ? std::string() : page.screenshots.front().word;
        return model(std::vector<slint::Image>{});
    }
    if (!any) {
        note = words.front();
        return model(std::vector<slint::Image>{});
    }
    const std::size_t at = static_cast<std::size_t>(std::clamp(pictureIndex_, 0, static_cast<int>(images.size()) - 1));
    note = captions[at];
    if (!words[at].empty()) { note += (note.empty() ? "" : "  ") + words[at]; }
    return model(std::move(images));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the models

::WebModel ExtendPublisher::mapWeb(const WebModel& m) {
    ::WebModel o{};
    o.serve = m.serve;
    o.reach = lists_->reach.set(items(m.reach));
    o.reach_index = m.reach_index;
    o.address = ss(m.address);
    o.port = ss(m.port);
    o.user = ss(m.user);
    o.password_status = ss(m.password_status);
    o.password_draft = ss(m.password_draft);
    o.confirm_draft = ss(m.confirm_draft);
    o.set_label = ss(m.set_label);
    o.clear_label = ss(m.clear_label);
    o.apply_label = ss(m.apply_label);
    o.apply_enabled = m.apply_enabled;
    o.error = ss(m.error);
    o.serving_note = ss(m.serving_note);
    o.network_warning = ss(m.network_warning);
    o.addresses = lists_->addresses.set(strings(m.addresses));
    o.http_note = ss(m.http_note);
    o.sessions = ss(m.sessions);
    o.listeners = ss(m.listeners);
    return o;
}

::CatModel ExtendPublisher::mapCat(const CatModel& m) {
    ::CatModel o{};
    o.intro = ss(m.intro);
    o.accept = m.accept;
    o.port = ss(m.port);
    o.reachable = m.reachable;
    o.warning = ss(m.warning);
    o.status = ss(m.status);
    o.status_tone = tone(m.status_tone);
    return o;
}

// Everything of the store model the window shows, as one string: the guard that keeps a model whose content did not move from being set again (a set list recreates every
// repeater item behind it, and the page's cards are three lists deep).
static std::string storeSig(const StoreModel& m, int pictureIndex, std::uint64_t pictureRevision) {
    std::string s;
    sigAdd(s, m.search); sigAdd(s, m.search_placeholder); sigAdd(s, m.tab); sigAdd(s, m.browse_label); sigAdd(s, m.updates_label);
    sigAdd(s, m.catalogue_line); sigAdd(s, static_cast<int>(m.catalogue_tone)); sigAdd(s, m.check_label); sigAdd(s, m.check_enabled); sigAdd(s, m.sites_open);
    for (const StoreSite& x : m.sites) { sigAdd(s, x.address); sigAdd(s, x.remove_label); sigAdd(s, x.remove_enabled); }
    for (const StoreLine& l : m.site_lines) { sigAdd(s, l.text); sigAdd(s, static_cast<int>(l.tone)); }
    sigAdd(s, m.no_sites_line); sigAdd(s, m.site_draft); sigAdd(s, m.consent); sigAdd(s, m.consent_sentence); sigAdd(s, m.add_site_label);
    sigAdd(s, m.top_error); sigAdd(s, m.top_warning); sigAdd(s, m.empty_line);
    for (const StoreCategory& c : m.categories) {
        sigAdd(s, c.heading);
        for (const StoreCard& k : c.cards) {
            sigAdd(s, k.id); sigAdd(s, k.glyph); sigAdd(s, k.name); sigAdd(s, k.summary); sigBadges(s, k.badges); sigAdd(s, k.key_label); sigAdd(s, k.key_kind);
            sigAdd(s, k.key_enabled); sigAdd(s, k.version_note); sigAdd(s, k.progress); sigAdd(s, static_cast<long long>(k.progress_norm * 1000.0f)); sigAdd(s, k.key_reason);
        }
        s += '\x1d';
    }
    sigAdd(s, m.get_all_label); sigAdd(s, m.get_all_enabled); sigAdd(s, m.get_all_reason); sigAdd(s, m.foot_note); sigAdd(s, m.bulk_tick); sigAdd(s, m.bulk_sentence);
    sigAdd(s, m.notice_note); sigAdd(s, m.left_out_note); sigAdd(s, m.add_all_summary); sigAdd(s, m.add_all_failed);
    sigAdd(s, m.updates_heading); sigAdd(s, m.update_all_label); sigAdd(s, m.update_all_enabled);
    for (const UpdateRow& u : m.updates) {
        sigAdd(s, u.id); sigAdd(s, u.glyph); sigAdd(s, u.name); sigAdd(s, u.version_note); sigAdd(s, u.badge); sigAdd(s, u.whats_new); sigAdd(s, u.key_label); sigAdd(s, u.key_enabled);
    }
    for (const std::string& f : m.footnotes) { sigAdd(s, f); }
    sigAdd(s, m.cleanup_report);
    sigAdd(s, m.cleanup_label); sigAdd(s, m.cleanup_enabled); for (const std::string& c : m.cleanup_lines) { sigAdd(s, c); }
    sigAdd(s, m.cleanup_open); sigAdd(s, m.cleanup_title); sigAdd(s, m.cleanup_text); sigAdd(s, m.cleanup_yes); sigAdd(s, m.cleanup_no);
    sigAdd(s, m.busy); sigAdd(s, m.cancel_label); sigAdd(s, m.site_error); sigAdd(s, m.add_site_reason);
    const StorePluginPage& p = m.plugin;
    sigAdd(s, p.shown); sigAdd(s, p.back_label); sigAdd(s, p.glyph); sigAdd(s, p.name); sigAdd(s, p.version); sigBadges(s, p.badges); sigAdd(s, p.origin);
    sigAdd(s, p.key_label); sigAdd(s, p.key_kind); sigAdd(s, p.key_enabled); sigAdd(s, p.report); sigAdd(s, static_cast<int>(p.report_tone));
    for (const PictureFrame& f : p.screenshots) { sigAdd(s, f.sha256); sigAdd(s, f.caption); sigAdd(s, f.state); sigAdd(s, f.word); sigAdd(s, f.path); }
    sigAdd(s, p.what_it_does); sigAdd(s, p.whats_new); sigAdd(s, p.details); sigAdd(s, p.details_open); sigAdd(s, p.details_label);
    sigAdd(s, p.notice_shown ? p.notice : std::string()); sigAdd(s, p.notice_tick); sigAdd(s, p.reach_lead);
    sigAdd(s, pictureIndex);
    sigAdd(s, static_cast<long long>(pictureRevision));
    return s;
}

::StoreModel ExtendPublisher::mapStore(const StoreModel& m) {
    ::StoreModel o{};
    o.search = ss(m.search);
    o.search_placeholder = ss(m.search_placeholder);
    o.tab = m.tab;
    o.browse_label = ss(m.browse_label);
    o.updates_label = ss(m.updates_label);
    o.catalogue_line = ss(m.catalogue_line);
    o.catalogue_tone = tone(m.catalogue_tone);
    o.check_label = ss(m.check_label);
    o.check_enabled = m.check_enabled;     // greyed while a transfer or a picture is in flight or no address is set (docs 6.61)
    o.sites_open = m.sites_open;

    // the sites: the UI's one status line a site is the AMBER one (its own colour is warn), so only the lines the logic calls Warn ride it - "could not be read" and the two
    // "hidden" notes. The neutral "<site>: 12 plugins" lines have no field to sit in. The logic's lines are in site order, the first of each site starting "<who>: ".
    std::vector<::StoreSite> sites;
    std::size_t line = 0;
    for (const StoreSite& x : m.sites) {
        ::StoreSite s{};
        s.address = ss(x.address);
        s.remove_label = ss(x.remove_label);
        s.remove_enabled = x.remove_enabled;   // greyed while a transfer runs (docs 6.61): the effect did nothing then, and the key looked alive
        std::string status;
        // a site's lines: its first, then the "hidden" ones that follow (they say " hidden: ")
        bool first = true;
        while (line < m.site_lines.size()) {
            const StoreLine& l = m.site_lines[line];
            const bool hidden = l.text.find(" hidden: ") != std::string::npos;
            if (!first && !hidden) { break; }
            if (l.tone == Tone::Warn) { status += (status.empty() ? "" : "\n") + l.text; }
            first = false;
            ++line;
        }
        s.status = ss(status);
        sites.push_back(std::move(s));
    }
    o.sites = model(std::move(sites));
    o.no_sites_line = ss(m.no_sites_line);
    o.site_draft = ss(m.site_draft);
    o.consent = m.consent;
    o.consent_sentence = ss(m.consent_sentence);
    o.add_site_label = ss(m.add_site_label);
    o.top_error = ss(m.top_error);
    o.top_warning = ss(m.top_warning);
    o.empty_line = ss(m.empty_line);

    std::vector<::StoreCategory> categories;
    for (const StoreCategory& c : m.categories) {
        std::vector<::StoreCard> cards;
        for (const StoreCard& k : c.cards) {
            ::StoreCard x{};
            x.id = ss(k.id);
            x.glyph = ss(glyphOf(k.name, k.glyph));
            x.name = ss(k.name);
            x.summary = ss(k.summary);
            x.badges = model(badges(k.badges));
            x.key_label = ss(k.key_label);
            x.key_kind = k.key_kind;
            x.key_enabled = k.key_enabled;
            x.version_note = ss(k.version_note);
            x.progress = ss(k.progress);
            x.progress_norm = k.progress_norm;
            x.key_tooltip = ss(k.key_reason);
            cards.push_back(std::move(x));
        }
        ::StoreCategory y{};
        y.heading = ss(c.heading);
        y.cards = model(std::move(cards));
        categories.push_back(std::move(y));
    }
    o.categories = model(std::move(categories));

    o.get_all_label = ss(m.get_all_label);
    o.get_all_enabled = m.get_all_enabled;
    o.get_all_reason = ss(m.get_all_reason);
    // The foot's one sentence: the run's progress or "Cannot add all: ...", then (while no run is going) the notice note, the sites-left-out note and what the last run did.
    std::string foot = m.foot_note;
    for (const std::string* extra : {&m.notice_note, &m.left_out_note, &m.add_all_summary}) {
        if (extra->empty() || (m.add_all_running && extra == &m.add_all_summary)) { continue; }
        foot += (foot.empty() ? "" : "\n") + *extra;
    }
    o.foot_note = ss(foot);
    o.bulk_tick = m.bulk_tick;
    o.bulk_sentence = ss(m.bulk_shown ? m.bulk_sentence : std::string());
    o.updates_heading = ss(m.updates_heading);
    o.update_all_label = ss(m.update_all_label);
    o.update_all_enabled = m.update_all_enabled;
    std::vector<::UpdateRow> updates;
    for (const UpdateRow& u : m.updates) {
        ::UpdateRow x{};
        x.id = ss(u.id);
        x.glyph = ss(glyphOf(u.name, u.glyph));
        x.name = ss(u.name);
        x.version_note = ss(u.version_note);
        x.badge = ss(u.badge);
        x.whats_new = ss(u.whats_new);
        x.key_label = ss(u.key_label);
        x.key_enabled = u.key_enabled;
        updates.push_back(std::move(x));
    }
    o.updates = model(std::move(updates));
    std::vector<std::string> notes = m.footnotes;
    if (!m.cleanup_report.empty()) { notes.push_back(m.cleanup_report); }   // what the last clean-up did (green in the ImGui window: the footnotes have one ink)
    o.footnotes = model(strings(notes));
    o.cleanup_label = ss(m.cleanup_label);
    o.cleanup_enabled = m.cleanup_enabled;
    o.cleanup_lines = model(strings(m.cleanup_lines));
    o.cleanup_open = m.cleanup_open;
    o.cleanup_title = ss(m.cleanup_title);
    o.cleanup_text = ss(m.cleanup_text);
    o.cleanup_yes = ss(m.cleanup_yes);
    o.cleanup_no = ss(m.cleanup_no);
    o.cancel_label = ss(m.busy ? m.cancel_label : std::string());
    o.site_error = ss(m.site_error);
    o.add_reason = ss(m.add_site_reason);

    const StorePluginPage& p = m.plugin;
    ::StorePluginPage g{};
    g.shown = p.shown;
    g.back_label = ss(p.back_label);
    g.glyph = ss(glyphOf(p.name, p.glyph));
    g.name = ss(p.name);
    g.version = ss(p.version);
    g.badges = model(badges(p.badges));
    g.origin = ss(p.origin);
    g.key_label = ss(p.key_label);
    g.key_kind = p.key_kind;
    g.key_enabled = p.key_enabled;
    g.report = ss(p.report);
    g.report_tone = tone(p.report_tone);
    std::string note;
    g.screenshots = p.shown ? mapScreenshots(p, note) : model(std::vector<slint::Image>{});
    g.pictures_note = ss(note);
    g.picture_index = std::max(0, std::min(pictureIndex_, static_cast<int>(g.screenshots->row_count()) - 1));
    g.what_it_does = ss(p.what_it_does);
    g.whats_new = ss(p.whats_new);
    g.details = ss(p.details);
    g.details_open = p.details_open;
    g.details_label = ss(p.details_label);
    g.notice = ss(p.notice_shown ? p.notice : std::string());
    g.notice_ticked = p.notice_tick;
    g.reach_lead = ss(p.details_open ? p.reach_lead : std::string());
    o.plugin = std::move(g);
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------------------
ReadOptions ExtendPublisher::buildOptions(const FoxReceiverState& state, bool readStore) {
    (void)state;
    ReadOptions o = opts_;
    o.read_store = readStore;
    o.now_unix = nowUnix_;
    return o;
}

bool ExtendPublisher::storeReadDue(const FoxReceiverState& state, std::int64_t nowMs) const {
    if (storeForce_ || !haveStore_) { return true; }
    if (state.listSeq != storeSeq_) { return true; }
    if (last_.store.busy && nowMs - lastStoreReadMs_ >= kStoreBusyEveryMs) { return true; }
    return nowMs - lastStoreReadMs_ >= kStoreEveryMs;
}

void ExtendPublisher::publish(const FoxReceiverState& state) {
    listSeq_ = state.listSeq;
    nowUnix_ = static_cast<std::int64_t>(std::time(nullptr));
    const std::int64_t nowMs = static_cast<std::int64_t>(nowS() * 1000.0);
    pollPictures();
    if (!probePaths_.empty() && !probeDone_) { runPictureProbe(); }
    // an answer the kit's poll took first is not coming: the draft stops waiting for it
    auto& asks = pending_->asks;
    for (auto it = asks.begin(); it != asks.end();) {
        if (nowS() - it->at > kAskExpiryS) {
            it = asks.erase(it);
            if (opts_.server_asks_pending > 0) { --opts_.server_asks_pending; }
            forceRead_ = true;
        } else {
            ++it;
        }
    }

    const foxsdr::slintkit::KitPublisher::StoreView view = pages_.storeView();
    const bool up = view != foxsdr::slintkit::KitPublisher::StoreView::Closed;
    const bool shown = view == foxsdr::slintkit::KitPublisher::StoreView::Shown;
    if (up && !pageWasUp_) { openedPending_ = true; storeForce_ = true; }
    if (!up) { openedPending_ = false; }
    if (shown && !shownWas_) { storeForce_ = true; }
    pageWasUp_ = up;
    shownWas_ = shown;

    const bool readStore = shown && storeReadDue(state, nowMs);
    // The lists and facts are read at 10 Hz (and at once after a gesture): the ImGui window reads them every frame, this one does not need a thirtieth of a second.
    if (!forceRead_ && !readStore && haveModels_ && nowMs - lastReadMs_ < 100) { return; }
    forceRead_ = false;
    lastReadMs_ = nowMs;

    const ReadOptions options = buildOptions(state, readStore);
    ExtendModels m = readExtend(table_, state, options);
    last_.web = m.web;
    last_.cat = m.cat;
    last_.chips = m.chips;
    haveModels_ = true;
    if (readStore) {
        last_.store = std::move(m.store);
        haveStore_ = true;
        storeForce_ = false;
        storeSeq_ = state.listSeq;
        lastStoreReadMs_ = nowMs;
        if (last_.store.draft_changed) { opts_.store = last_.store.draft; }   // a page the catalogue no longer lists was closed
        const ConsentEdge edge = watchConsents(consents_, last_.store);
        if (edge.any()) {
            if (edge.clear_notice_tick) { opts_.store.notice_tick = false; }
            if (edge.clear_bulk_tick) { opts_.store.bulk_tick = false; }
            storeForce_ = true;   // the next read shows the ticks cleared
        }
        // the strip starts again on a page of its own
        if (last_.store.plugin.shown && pictureIndexPage_ != last_.store.plugin.id) {
            pictureIndexPage_ = last_.store.plugin.id;
            pictureIndex_ = 0;
        } else if (!last_.store.plugin.shown) {
            pictureIndexPage_.clear();
            pictureIndex_ = 0;
        }
        // a bounded run's capture of a plugin's page: the draft is set, nothing is asked of the network
        if (!captureId_.empty()) {
            const auto& ids = last_.store.ids;
            if (std::find(ids.begin(), ids.end(), captureId_) != ids.end()) {
                StoreDraft d = opts_.store;
                d.page_id = captureId_;
                d.tab = 0;
                d.details_open = true;
                d.notice_tick = false;
                opts_.store = d;
                last_.store.draft = d;
                captureId_.clear();
                storeForce_ = true;
            } else if (++captureWaits_ > kCaptureTicks) {
                say("FOXSDR_SLINT_STORE_PAGE: the catalogue does not list \"" + captureId_ + "\"; the page stays on the grid");
                captureId_.clear();
            }
        }
        // the one automatic read of the catalogue the first time the page is up in a session, then the pictures its open page lists and nobody asked for
        if (openedPending_) {
            openedPending_ = false;
            exec(applyStoreOpened(last_.store, firstOpen_, last_.store.read_this_session), "store-opened");
        }
        if (picturesAskedSeq_ != state.listSeq && last_.store.plugin.shown) {
            const Effect top = applyStorePicturesTop(last_.store);
            if (!top.empty()) { exec(top, "store-pictures-top"); picturesAskedSeq_ = state.listSeq; }
        }
        wantPictures(last_.store.plugin);

        // FOXSDR_SLINT_STORE_LOCAL: the address is set to the local index (its own ops, not an effect's), then - a few ticks on, the engine having applied it - the read is asked
        if (!localCatalogue_.empty() && localStep_ < 2) {
            if (localStep_ == 0) {
                kit_.submit(cc::cmd::makeText(FOXAPP_OP_SET_CATALOGUE_URL, localCatalogue_));
                say("capture - the catalogue address is a local file; the read is asked in a few ticks");
                localStep_ = 1;
            } else if (++localWait_ >= 6) {
                kit_.submit(cc::cmd::make(FOXAPI_OP_STORE_FETCH));
                say("capture - the local catalogue is read (no network: the engine reads the file)");
                localStep_ = 2;
            }
            storeForce_ = true;
        }
        // hand the page its model, when its content moved
        const std::string sig = storeSig(last_.store, pictureIndex_, pictures_->revision);
        if (!lastSet_->haveStoreSet || sig != lastSet_->storeSig) {
            lastSet_->haveStoreSet = true;
            lastSet_->storeSig = sig;
            pages_.setStoreModel(mapStore(last_.store));
        }
    }

    // the DECODE row's chip (the rail tier: read whether the page is up or not)
    {
        const StoreRailChip c = readStoreChip(table_);
        const ::ChipTone t = (!c.lit && c.word == tr("IDLE")) ? ::ChipTone::Neutral : tone(c.tone);
        pages_.setStoreRail(c.word, t, c.lit);
    }

    const auto setIf = [](auto& slot, auto&& value, auto&& setter) {
        if (!slot.has_value() || !(*slot == value)) {
            slot = value;
            setter(value);
        }
    };
    setIf(lastSet_->web, mapWeb(last_.web), [&](const ::WebModel& v) { ui_.set_web_model(v); });
    setIf(lastSet_->cat, mapCat(last_.cat), [&](const ::CatModel& v) { ui_.set_cat_model(v); });
}

ExtendPublisher::RowFace ExtendPublisher::face(int row) const {
    RowFace f;
    if (!haveModels_ || row < 0 || row >= kExtendRowCount) { return f; }
    const RowChip& c = last_.chips[static_cast<std::size_t>(row)];
    f.chip = c.word;
    f.valid = !c.word.empty();
    f.tone = c.tone == Tone::Err ? ::ChipTone::Err : c.lit ? ::ChipTone::Good : ::ChipTone::Neutral;
    return f;
}

// The self-capture of the worker: a synthetic page of Ready frames, one a file; once every file has been through the decode, the strip's mapping is read back at each index.
void ExtendPublisher::runPictureProbe() {
    StorePluginPage page;
    page.shown = true;
    for (std::size_t i = 0; i < probePaths_.size(); ++i) {
        PictureFrame f;
        f.sha256 = "probe-" + std::to_string(i);
        f.caption = "probe frame " + std::to_string(i);
        f.state = kPicReady;
        f.path = probePaths_[i];
        page.screenshots.push_back(std::move(f));
    }
    wantPictures(page);
    for (const PictureFrame& f : page.screenshots) {
        if (pictures_->done.count(f.sha256) == 0) { return; }
    }
    const int saved = pictureIndex_;
    std::string line = "picture probe:";
    for (std::size_t i = 0; i < page.screenshots.size(); ++i) {
        pictureIndex_ = static_cast<int>(i);
        std::string note;
        const auto images = mapScreenshots(page, note);
        const auto& entry = pictures_->done[page.screenshots[i].sha256];
        std::string size = "no image";
        if (entry.error.empty()) {
            const auto s = entry.image.size();
            size = std::to_string(s.width) + "x" + std::to_string(s.height);
        }
        line += " [" + std::to_string(i) + "] " + size + " (strip " + std::to_string(images->row_count()) + " frames, note \"" + note + "\")";
    }
    pictureIndex_ = saved;
    probeLine_ = line;
    probeDone_ = true;
    say(line);
}

void ExtendPublisher::probePictures(const std::vector<std::string>& paths) {
    probePaths_ = paths;
    probeDone_ = false;
}

void ExtendPublisher::useLocalCatalogueForCapture(const std::string& path) {
    if (!bounded_ || path.empty() || path.find("://") != std::string::npos || !std::filesystem::exists(std::filesystem::path(std::u8string(path.begin(), path.end())))) {
        say("FOXSDR_SLINT_STORE_LOCAL: not a local file (or not a bounded run) - nothing is read");
        return;
    }
    localCatalogue_ = path;
    localStep_ = 0;
    localWait_ = 0;
}

void ExtendPublisher::openStorePageForCapture(const std::string& id) {
    captureId_ = id;
    captureWaits_ = 0;
    storeForce_ = true;
}

std::string ExtendPublisher::summaryLine() const {
    if (!haveModels_) { return "extend: no models read"; }
    using View = foxsdr::slintkit::KitPublisher::StoreView;
    const View v = pages_.storeView();
    std::string s = "extend: web \"" + last_.web.chip + "\" " + (last_.web.serve ? "serve=on" : "serve=off") + " port=" + last_.web.port + " user=\"" + last_.web.user + "\" \"" +
                    last_.web.password_status + "\" note=\"" + (last_.web.error.empty() ? last_.web.serving_note : last_.web.error) + "\"; cat \"" + last_.cat.chip + "\" " +
                    (last_.cat.accept ? "accept=on" : "accept=off") + " port=" + last_.cat.port + " status=\"" + last_.cat.status + "\"; store page=" +
                    (v == View::Closed ? "closed" : v == View::Docked ? "docked" : "shown");
    if (!probeLine_.empty()) { s += "; " + probeLine_; }
    if (haveStore_) {
        std::size_t cards = 0;
        for (const StoreCategory& c : last_.store.categories) { cards += c.cards.size(); }
        s += " catalogue=\"" + last_.store.catalogue_line + "\" cards=" + std::to_string(cards) + " categories=" + std::to_string(last_.store.categories.size()) +
             " ids=" + std::to_string(last_.store.ids.size()) + " updates=" + std::to_string(last_.store.update_count) + " sites=" + std::to_string(last_.store.sites.size()) +
             " tab=" + (last_.store.tab == 0 ? "browse" : "updates") + " open page=\"" + (last_.store.plugin.shown ? last_.store.plugin.name : std::string()) + "\"";
        if (!last_.store.empty_line.empty()) { s += " empty=\"" + last_.store.empty_line + "\""; }
        s += " pictures decoded=" + std::to_string(pictures_->done.size());
    } else {
        s += " (not read: the page has not been up)";
    }
    return s + "; commands sent=" + std::to_string(sent_) + " withheld=" + std::to_string(withheld_);
}

// ---------------------------------------------------------------------------------------------------------------------------------
void ExtendPublisher::wire() {
    auto&& ev = ui_.global<::ExtendEvents>();

    // ---- Web access (the password boxes' text goes to the effect and nowhere else: no line here names it) ----
    ev.on_web_serve([this](bool on) { exec(applyWebServe(last_.web, on), "web-serve"); });
    ev.on_web_reach([this](int i) { exec(applyWebReach(last_.web, i), "web-reach"); });
    ev.on_web_port([this](slint::SharedString t) { exec(applyWebPort(last_.web, std::string(t)), "web-port"); });
    ev.on_web_user([this](slint::SharedString t) { exec(applyWebUser(last_.web, std::string(t)), "web-user"); });
    ev.on_web_password([this](slint::SharedString t) { exec(applyWebPassword(last_.web, std::string(t)), "web-password"); });
    ev.on_web_confirm([this](slint::SharedString t) { exec(applyWebConfirm(last_.web, std::string(t)), "web-confirm"); });
    ev.on_web_set_password([this] {
        exec(applyWebSetPassword(last_.web), "web-set-password");
        // the boxes the last read held are the ones just sent: gone from here too (the next read takes the wiped draft)
        std::fill(last_.web.password_draft.begin(), last_.web.password_draft.end(), '\0');
        std::fill(last_.web.confirm_draft.begin(), last_.web.confirm_draft.end(), '\0');
        last_.web.password_draft.clear();
        last_.web.confirm_draft.clear();
    });
    ev.on_web_clear_password([this] { exec(applyWebClearPassword(last_.web), "web-clear-password"); });
    ev.on_web_apply([this] { exec(applyWebApply(last_.web), "web-apply"); });

    // ---- CAT control ----
    ev.on_cat_accept([this](bool on) { exec(applyCatAccept(last_.cat, on), "cat-accept"); });
    ev.on_cat_port([this](slint::SharedString t) { exec(applyCatPort(last_.cat, std::string(t)), "cat-port"); });
    ev.on_cat_reachable([this](bool on) { exec(applyCatReachable(last_.cat, on), "cat-reachable"); });

    // ---- the Plugin store ----
    ev.on_store_search([this](slint::SharedString t) { exec(applyStoreSearch(last_.store, std::string(t)), "store-search"); });
    ev.on_store_tab([this](int i) { exec(applyStoreTab(last_.store, i), "store-tab"); });
    ev.on_store_check([this] { exec(applyStoreCheck(last_.store), "store-check"); });
    ev.on_store_sites_toggle([this] { exec(applyStoreSitesToggle(last_.store), "store-sites-toggle"); });
    ev.on_store_site_remove([this](int i) { exec(applyStoreSiteRemove(last_.store, i), "store-site-remove"); });
    ev.on_store_site_draft([this](slint::SharedString t) { exec(applyStoreSiteDraft(last_.store, std::string(t)), "store-site-draft"); });
    ev.on_store_consent([this](bool on) { exec(applyStoreConsent(last_.store, on), "store-consent"); });
    ev.on_store_site_add([this] { exec(applyStoreSiteAdd(last_.store), "store-site-add"); });
    ev.on_store_card_key([this](slint::SharedString id) { exec(applyStoreCardKey(last_.store, std::string(id)), "store-card-key"); });
    ev.on_store_card_open([this](slint::SharedString id) { exec(applyStoreCardOpen(last_.store, std::string(id)), "store-card-open"); });
    ev.on_store_get_all([this] { exec(applyStoreGetAll(last_.store), "store-get-all"); });
    ev.on_store_bulk_tick([this](bool on) { exec(applyStoreBulkTick(last_.store, on), "store-bulk-tick"); });
    ev.on_store_update_all([this] { exec(applyStoreUpdateAll(last_.store), "store-update-all"); });
    ev.on_store_update([this](slint::SharedString id) { exec(applyStoreUpdate(last_.store, std::string(id)), "store-update"); });
    ev.on_store_cleanup([this] { exec(applyStoreCleanup(last_.store), "store-cleanup"); });
    ev.on_store_cleanup_confirm([this](bool yes) { exec(applyStoreCleanupConfirm(last_.store, yes), "store-cleanup-confirm"); });
    ev.on_store_plugin_back([this] { exec(applyStorePluginBack(last_.store), "store-plugin-back"); });
    ev.on_store_plugin_key([this] { exec(applyStorePluginKey(last_.store), "store-plugin-key"); });
    ev.on_store_plugin_details([this](bool on) { exec(applyStorePluginDetails(last_.store, on), "store-plugin-details"); });
    // The strip's arrows: the effect is empty (the ImGui strip has no press), the index is this object's own - the page the logic gave is one, this keeps which frame shows.
    ev.on_store_picture([this](int i) {
        exec(applyStorePicture(last_.store, i), "store-picture");
        int count = 0;
        for (const PictureFrame& f : last_.store.plugin.screenshots) { count += f.sha256.empty() ? 0 : 1; }
        pictureIndex_ = std::max(0, std::min(i, std::max(0, count - 1)));
        storeForce_ = true;
    });
    // The two the UI family added (the contract has no callback for them): CANCEL, and the maker's notice tick.
    ev.on_store_cancel([this] { exec(applyStoreCancel(last_.store), "store-cancel"); });
    ev.on_store_plugin_notice([this](bool on) { exec(applyStoreNoticeTick(last_.store, on), "store-plugin-notice"); });
}

}  // namespace foxsdr::slintextend
