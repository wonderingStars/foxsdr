// The web server itself (P11): an HTTP listener that serves a browser client
// for the receiver, gated by net/web_policy.hpp's bind decision and
// net/web_auth.hpp's credentials.
//
// WHAT IT IS NOT COUPLED TO. This class knows nothing about Pipeline, the DSP
// chain, or the GUI. It obtains everything it serves through the two provider
// callbacks below, which the application fills in from the pipeline. That keeps
// the DSP headers out of every translation unit that touches the server, lets
// the tests drive it with stubs instead of a running radio, and means the
// server can never reach into the pipeline for something it was not explicitly
// given.
//
// THREADING. start() spawns one thread that owns the listener; the HTTP library
// serves each request on its own worker. The provider callbacks are therefore
// called from arbitrary threads and MUST be safe to call concurrently — the
// pipeline's own getters already are, which is why they can be wired straight
// in. stop() joins the listener thread before returning, so no handler can be
// running once it has.
//
// TRANSPORT SECURITY. There is none: this speaks plain HTTP, deliberately (see
// third_party/THIRD_PARTY.md for why OpenSSL is not linked). That single fact
// drives the whole design here — the bind policy refuses an off-machine bind
// without a password, the session cookie is SameSite=Strict, and reaching this
// from beyond the LAN is documented as "terminate TLS in front of it", never
// "forward the port".
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/web_audio.hpp"
#include "net/web_auth.hpp"
#include "net/web_control.hpp"
#include "net/web_policy.hpp"

// Forward-declared rather than included: httplib.h is 747 KB and belongs in
// exactly one translation unit.
namespace httplib {
class Server;
}

namespace cascade::net {

// Fixed display range for the spectrum sent to the browser. Fixed rather than
// per-frame min/max because a range recomputed each frame makes the waterfall
// breathe: the same signal changes colour as the noise floor wanders.
inline constexpr float kSpectrumDbMin = -140.0f;
inline constexpr float kSpectrumDbMax = 0.0f;

// One consistent snapshot of what the receiver is doing. Assembled by the
// application under whatever locking the pipeline needs, so the browser can
// never show two fields from different instants.
struct RadioStatus {
    bool running = false;
    bool faulted = false;
    std::string faultMessage;
    double centerHz = 0.0;
    double sampleRateHz = 0.0;
    double vfoOffsetHz = 0.0;
    double bandwidthHz = 0.0;
    std::string mode;
    std::string sourceName;
    float signalDb = -200.0f;
    bool stereoActive = false;
    // Included so the browser's own controls can show where they currently
    // sit. Without them the page would have to remember what it last sent,
    // which goes wrong the moment the desktop window changes something.
    float squelchDb = 0.0f;
    float volume = 0.0f;

    // --- Display ---------------------------------------------------------
    // The dB range the desktop's spectrum axis and waterfall colormap share.
    // The browser needs it for the same reason: a waterfall drawn against a
    // different range from the one the window uses is a different picture of
    // the same signal.
    float dbMin = -110.0f;
    float dbMax = 0.0f;

    // --- Audio processing ------------------------------------------------
    int deemphasisIndex = 0;   // 0 = 50 us, 1 = 75 us, 2 = off
    bool nrEnabled = false;
    float nrStrength = 0.5f;
    bool notchEnabled = false;
    double notchFreqHz = 1000.0;
    double notchQ = 30.0;
    bool autoNotch = false;
    bool autoNotchEngaged = false;
    double autoNotchFreqHz = 0.0;

    // --- Broadcast FM ------------------------------------------------------
    bool stereoEnabled = true;
    bool pilotLocked = false;

    // --- RDS ---------------------------------------------------------------
    // Empty/zero when nothing is decoded, which is every mode but WFM and the
    // first seconds of most WFM stations.
    //
    // rdsPty is the RAW 5-bit programme-type CODE, not a name, because the
    // code-to-name table differs between RDS (Europe) and RBDS (North
    // America) — dsp/rds.hpp exposes the code for exactly that reason and the
    // browser must not invent one of the two tables.
    bool rdsSynced = false;
    bool rdsPiValid = false;
    unsigned rdsPi = 0;
    bool rdsPsValid = false;
    std::string rdsPs;           // the 8-character Programme Service name
    std::string rdsRadioText;
    unsigned rdsPty = 0;
    bool rdsTp = false;
    bool rdsTa = false;
    unsigned rdsGroups = 0;
    unsigned rdsErrors = 0;

    // --- Source ------------------------------------------------------------
    struct SoapyDevice {
        std::string label;
        std::string args;
    };
    struct GainStage {
        std::string name;
        double db = 0.0;
    };

    std::string sourceKind = "siggen";   // "siggen" | "file" | "soapy"
    std::string soapyArgs;               // kwargs of the open device, if any
    std::string antenna;                 // RX port the DRIVER reports
    std::vector<std::string> antennas;   // ports it offers
    std::vector<SoapyDevice> devices;    // last enumeration, empty until asked
    std::vector<GainStage> gains;
    bool agcSupported = false;
    bool agc = false;
    // True while a device scan or open is in flight, so the page can say so
    // rather than looking as though the button did nothing — both take
    // seconds (a scan walks the USB bus; an open loads FPGA firmware).
    bool sourceBusy = false;
    // The application's own last source error, verbatim. Empty when fine.
    std::string sourceError;

    // --- Recorder ----------------------------------------------------------
    bool iqRecording = false;
    // WHO IS SILENCING THE AUDIO STREAM, or empty when nobody is. The mute is
    // applied in the pipeline, before the tap this server's audio is pumped
    // from, so a browser listening while a data decoder runs on its preset
    // receives real silence - which without this line is indistinguishable
    // from a broken stream. Names the plugins, exactly as the desktop's Sinks
    // panel does, so the two clients cannot tell different stories about the
    // same radio.
    std::string audioMutedBy;
    bool audioRecording = false;
    std::uint64_t iqBytes = 0;
    std::uint64_t audioBytes = 0;
    std::string recordDir;
    std::string recordError;

    // --- Bookmarks ---------------------------------------------------------
    struct Bookmark {
        std::string name;
        double freqHz = 0.0;
        std::string mode;
        double bandwidthHz = 0.0;
    };
    std::vector<Bookmark> bookmarks;

    // --- Scanner -----------------------------------------------------------
    bool scannerActive = false;
    std::string scannerState;   // "idle" | "scanning" | "paused" | "holding"
    double scanStartHz = 0.0;
    double scanStopHz = 0.0;
    double scanStepHz = 0.0;

    // --- Decoder output ----------------------------------------------------
    // The tail of what the loaded plugins have decoded. A TAIL, not an
    // archive: the recorder is where a permanent copy belongs, and shipping
    // the whole log on every status poll would grow without bound.
    struct DecodedLine {
        std::string plugin;
        std::string text;
    };
    std::vector<DecodedLine> decoded;

    // --- Tracks ------------------------------------------------------------
    // Decoded targets — aircraft, vessels, stations, satellites — as the map
    // and the track list show them. altM/courseDeg/speedMps are NaN when the
    // source does not report them (0 is a real altitude and a real course), and
    // the JSON carries null for those rather than a number that would put every
    // unknown-altitude aircraft on the ground.
    struct Track {
        std::string id;
        std::string label;
        std::string plugin;
        double latDeg = 0.0;
        double lonDeg = 0.0;
        double altM = 0.0;
        double courseDeg = 0.0;
        double speedMps = 0.0;
        std::uint64_t ageMs = 0;
        unsigned kind = 0;
        unsigned flags = 0;
        // Who this is, when a track-info plugin has answered: 0 = no answer
        // yet (or no plugin), 1 = known, 2 = the source does not have it.
        // Separate states because "still looking" and "will never know" read
        // differently to a person.
        unsigned infoState = 0;
        std::string registration;
        std::string acType;      // "Airbus A319-111", or the type code
        std::string acOperator;
        std::string acCountry;
    };
    std::vector<Track> tracks;

    // WHERE THE ANTENNA IS. Published because a range-and-bearing display -
    // the radar unit is one - cannot draw a single ring without it, and
    // because "the user has not told us" and "the user is at 0N 0E" are
    // different answers that must not look the same. rxPositionSet is the
    // one that decides; the two degrees mean nothing while it is false.
    bool rxPositionSet = false;
    double rxLatDeg = 0.0;
    double rxLonDeg = 0.0;

    // --- Plugins -----------------------------------------------------------
    // What is installed, and for anything refused, why. Read-only: this page
    // deliberately cannot install or remove a plugin — that moves NATIVE CODE
    // onto the machine, which is a different question from tuning a radio and
    // one the owner should answer at the application, not over HTTP.
    struct Plugin {
        std::string name;
        std::string version;
        std::string licence;
        std::string fileName;   // what pluginRemove names
        bool loaded = false;
        std::string error;      // empty iff loaded
        std::string idleReason; // why it is loaded but not being fed, if so
        // STOPPED BY THE USER: loaded, listed, and deliberately running
        // nothing. Reported because the browser would otherwise show a stopped
        // plugin exactly like a running one and claim it was working - which
        // is the same "installed and silent with no explanation" the desktop's
        // idle reasons exist to prevent.
        bool stopped = false;
        // Declares CASCADE_CAP_HOST_CLIENT, i.e. it can ASK to move the
        // receiver — and whether the user has said yes.
        bool canRequestTune = false;
        bool tuneAllowed = false;
        // Where this decoder listens, as it declares through
        // CASCADE_CAP_PRESET. One click each: a decoder knows its own
        // frequency and the user usually does not, and making somebody find
        // out that ADS-B is at 1090 MHz wanting 2 MS/s of raw band is the
        // difference between a plugin that works when you click it and one
        // that appears to do nothing.
        struct Preset {
            std::string label;
            double frequencyHz = 0.0;
            double bandwidthHz = 0.0;
            double sampleRateHz = 0.0;
        };
        std::vector<Preset> presets;
    };
    std::vector<Plugin> plugins;

    // --- Plugin catalogue --------------------------------------------------
    // Empty until someone presses Browse. NOTHING here causes a fetch: the
    // published catalogue's promise is that the application does not contact
    // the origin unasked, and a status poll must not become the thing that
    // breaks it.
    struct CatalogEntry {
        std::string id;
        std::string name;
        std::string version;
        std::string licence;
        std::string summary;
        std::string legalNotice;   // must be acknowledged before Install
        bool installed = false;
        // Empty when Install may be pressed; otherwise the reason it may not,
        // from the SAME predicate the desktop's button uses.
        std::string blockedReason;
    };
    std::vector<CatalogEntry> catalogue;
    std::string catalogueStatus;
    std::string catalogueError;
    bool catalogueBusy = false;    // a fetch or a download is in flight
    std::string installReport;     // last success, verbatim
    std::string installError;      // last failure, verbatim

    // --- Basemap -----------------------------------------------------------
    // Whether a basemap plugin is supplying map imagery, and what the browser
    // needs to draw it: the zoom range and tile size to plan fetches, and the
    // attribution it is REQUIRED to show while any tile is on screen — the
    // same ODbL obligation the desktop enforces at plugin load, which must not
    // be lost in transit to a different renderer.
    struct Basemap {
        bool active = false;
        std::string attribution;
        unsigned minZoom = 0;
        unsigned maxZoom = 19;
        unsigned tileSize = 256;
    };
    Basemap basemap;

    // --- Decoded images ----------------------------------------------------
    // METADATA only. The pixels are fetched separately from /api/image/<n>,
    // because a decoded picture is hundreds of kilobytes and the status poll
    // runs four times a second — inlining one would make the whole readout
    // stall behind it. `revision` is what tells a browser the picture changed,
    // so it refetches once per CHANGE rather than once per poll.
    struct Image {
        std::string plugin;
        unsigned width = 0;
        unsigned height = 0;
        bool complete = false;
        std::uint64_t revision = 0;
    };
    std::vector<Image> images;
};

// One decoded picture, already encoded as a 24-bit BMP, ready to serve.
struct WebImage {
    std::string plugin;
    unsigned width = 0;
    unsigned height = 0;
    bool complete = false;
    std::uint64_t revision = 0;
    std::vector<std::uint8_t> bmp;
};

struct SpectrumSnapshot {
    std::vector<float> dbBins;   // fftshifted dB power, oldest-to-newest bin order
    std::uint64_t seq = 0;       // strictly increasing; 0 means nothing yet
    double centerHz = 0.0;
    double spanHz = 0.0;
};

// This machine's own IPv4 addresses, so the settings panel can say "open
// http://192.168.1.20:8073 on your phone" instead of leaving the user to find
// it. Loopback is excluded; an empty result means enumeration failed and the
// caller should simply omit the hint.
//
// It lives here rather than in the GUI because every line of socket code in
// this product belongs in the one translation unit that already owns the
// socket headers and their initialisation.
std::vector<std::string> localInterfaceAddresses();

class WebServer {
public:
    using StatusProvider = std::function<RadioStatus()>;
    // Returns false when no frame newer than `inOut.seq` exists, exactly like
    // Pipeline::getLatestFrame, so the browser can long-poll without copying
    // the same frame twice.
    using SpectrumProvider = std::function<bool(SpectrumSnapshot& inOut)>;
    using Clock = std::function<std::int64_t()>;  // seconds since the epoch

    WebServer();
    ~WebServer();  // stops if running

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // Must be set before start(); calling them while running is a no-op that
    // returns false, because a handler could be reading the old one.
    bool setStatusProvider(StatusProvider fn);
    bool setSpectrumProvider(SpectrumProvider fn);

    // Overrides the wall clock, for tests that need session expiry and login
    // throttling to be deterministic. Must be set before start().
    bool setClock(Clock fn);

    // Evaluates the bind policy and, if it allows, binds and starts serving.
    //
    // Returns false with `error` set when the policy refuses (error is the
    // policy's own reason text, suitable for showing verbatim) or when the
    // socket cannot be bound (address already in use, permission denied). The
    // two are distinguishable through decision(): a policy refusal leaves a
    // non-Allowed verdict, a bind failure leaves Allowed.
    //
    // Idempotent in the sense that starting an already-running server stops it
    // first, so applying new settings is one call rather than a stop/start
    // dance the caller has to get right.
    bool start(const WebServerConfig& cfg, std::string& error);

    void stop();
    bool running() const;

    // The port actually bound, which is the configured one unless the config
    // asked for 0. -1 when not running.
    int boundPort() const;

    // The most recent policy verdict, including reachableOffMachine for the
    // "remote access is live" indicator.
    BindDecision decision() const;

    // Live session count, for the settings panel ("2 browsers connected").
    std::size_t sessionCount() const;

    // Ends every session without stopping the server. The settings UI calls
    // this on a password change.
    void revokeAllSessions();

    // --- Control -------------------------------------------------------------
    // POST /api/control validates a request (net/web_control.hpp) and queues
    // it HERE; it never touches the radio, because several of the things a
    // browser can ask for are GUI-thread-only. The application drains this
    // once per frame and applies what it finds.
    //
    // Returns everything accepted since the last call, oldest first, and
    // empties the queue. Safe to call from the application's own thread while
    // requests keep arriving.
    std::vector<ControlRequest> takePendingControls();

    // Bound on the queue. An application that stops draining (a stalled GUI
    // thread) must not let a client grow this without limit; past the cap the
    // OLDEST request is dropped, because for a control surface the most recent
    // instruction is the one that matters.
    static constexpr std::size_t kMaxQueuedControls = 64;

    // --- Audio ---------------------------------------------------------------
    // Publishes newly produced receiver audio (mono, kWebAudioRateHz) for any
    // browser currently listening. The application calls this once per frame
    // with exactly the samples produced since the previous call; the ring
    // (net/web_audio.hpp) gives each listener its own cursor.
    //
    // Safe to call whether or not anyone is listening, and whether or not the
    // server is running — it is a buffer write, not a send.
    void pushAudio(const float* samples, std::size_t n);

    // Browsers currently streaming audio, for the settings panel.
    std::size_t audioListeners() const;

    // Replaces the served set of decoded pictures. The application calls this
    // only when a decoder's revision actually moves — re-encoding a megapixel
    // BMP every frame would cost more than the whole rest of the server.
    void setImages(std::vector<WebImage> images);

    // --- Basemap tiles -------------------------------------------------------
    // GET /api/tile/{z}/{x}/{y} is demand-driven by the BROWSER's viewport,
    // which this process cannot predict — but the plugin that has the tiles
    // may only be called on the GUI thread (the ABI promises nothing about any
    // other). So the route never calls the plugin: a request for a tile the
    // server does not hold is RECORDED and answered 202, the application
    // drains the recordings once per frame, fetches from the plugin on the
    // thread the ABI requires, and publishes the result here; the browser
    // simply asks again. A tile that is held is served as image/bmp; one
    // published as missing answers 404 and the browser stops asking.

    struct TileRequest {
        std::uint32_t z = 0;
        std::uint32_t x = 0;
        std::uint32_t y = 0;
    };

    // Tiles browsers asked for that the server could not serve, deduplicated,
    // oldest first; empties the queue. GUI thread, once per frame.
    std::vector<TileRequest> takePendingTileRequests();

    // Publishes one tile. An EMPTY body records "missing", which is a real
    // answer (the plugin said the tile will never exist) and not an absence —
    // it is what stops the browser retrying a tile nobody can produce.
    void setTile(std::uint32_t z, std::uint32_t x, std::uint32_t y,
                 std::vector<std::uint8_t> bmp);

    // Drops every stored tile AND every pending request. The application calls
    // this when the basemap plugin changes or goes away: tiles from the old
    // source must not be served as if the new one produced them.
    void clearTiles();

    // Bounds. Requests beyond the queue cap are dropped rather than queued —
    // the browser retries, so a lost recording costs a round trip, not a tile.
    // The store cap evicts the least recently SERVED tile; at 256 tiles of
    // 192 KB the store tops out near 50 MB.
    static constexpr std::size_t kMaxQueuedTileRequests = 64;
    static constexpr std::size_t kMaxStoredTiles = 256;

    // Each listener holds one HTTP worker thread for as long as it listens, so
    // this is capped well below the library's pool. Past the cap the request is
    // refused with 503 rather than being accepted and starving the rest of the
    // API — a page that cannot fetch its own status is a worse failure than one
    // that cannot play audio.
    static constexpr std::size_t kMaxAudioListeners = 4;

private:
    class Impl;  // holds the httplib::Server and the route handlers

    std::unique_ptr<Impl> impl_;
};

}  // namespace cascade::net
