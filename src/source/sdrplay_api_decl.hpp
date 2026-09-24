// sdrplay_api_decl.hpp - OUR OWN description of the SDRplay API's binary
// interface, written from the vendor's public header so that FoxSDR can drive
// an RSP through the user's own installed API without linking to anything of
// SDRplay's at build time and without shipping a byte of it.
//
// WHY A HAND-WRITTEN INTERFACE DESCRIPTION AT ALL. Every other radio in this
// application is reached by speaking its USB protocol directly
// (src/usb/usb_device.hpp). An RSP cannot be: SDRplay do not publish the
// device protocol, the tuner is programmed by a Windows SERVICE that owns the
// USB handle, and the only documented way in is sdrplay_api.dll talking to
// that service. So "native" here means what it can mean - we load the user's
// own DLL at runtime, resolve the entry points we use, and drive the API
// ourselves. No SoapySDR module, no vendor library linked, nothing of theirs
// redistributed. The declarations below exist only so the compiler knows the
// shape of the memory the service hands back.
//
// WHICH IS THE WHOLE RISK, AND WHY EVERY STRUCT BELOW IS PINNED BY A
// static_assert. sdrplay_api_GetDeviceParams returns POINTERS INTO MEMORY THE
// SERVICE ALLOCATED. We write the tune, the gain, the sample rate and the
// antenna selection straight into those structs. If one field of ours sits at
// a different offset from the service's, the write lands on the wrong field -
// silently, at run time, on somebody else's hardware, with no error anywhere.
// A sizeof/offsetof mismatch must therefore be a COMPILE failure here, not a
// field report from a stranger whose bias tee came on when he asked for an
// antenna.
//
// WHAT THESE WERE CHECKED AGAINST. The public sdrplay_api headers for API
// 3.07, 3.11 and 3.15 were fetched and COMPILED with this repository's own
// compiler (MSVC 19.44, x64), and every number asserted below is what that
// compiler reported for the vendor's own declarations - not arithmetic done by
// hand and hoped over. The three versions agree on the size and the offset of
// everything this driver touches, with exactly two differences, both of which
// this file handles explicitly:
//
//   1. DeviceT::valid does not exist before 3.08; at 3.07 those bytes are
//      padding. sizeof(DeviceT) is 96 either way and every other field is at
//      the same offset, so the struct below is correct for 3.07 too - but
//      `valid` must not be BELIEVED below 3.08 (kValidFieldSinceVersion).
//   2. RspDuoTunerParamsT grew a resetSlaveFlags member after 3.11 (present at
//      3.15, absent at 3.07 and 3.11): 12 bytes becomes 16. sizeof of the
//      enclosing RxChannelParamsT is 144 in BOTH layouts - the growth is
//      absorbed by trailing padding - and every field this driver writes is
//      at the same offset in both, with ONE exception: rspDxTunerParams moves
//      from 136 to 140. That member holds only the RSPdx HDR bandwidth, so
//      the driver simply does not write it below 3.15
//      (kRspDxTunerLayoutVersion) rather than guess at the layout of the
//      3.12-3.14 releases, whose headers were not available to check.
//
// Nothing in the range 3.12-3.14 was verifiable, which is precisely why the
// one member whose offset moved is gated on 3.15 rather than on "not 3.11".
//
// The call sequence this interface is used in was learned from SoapySDRPlay3
// (MIT, Charles J. Cliffe / Franco Venturi): Registration.cpp, Settings.cpp,
// Streaming.cpp. None of its code is here; what is here is the vendor's own
// published interface, restated.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <mutex>
#include <string>

namespace cascade::source::sdrplay_abi {

// --- versions -------------------------------------------------------------

// What the structs below were written and asserted against.
inline constexpr float kDeclaredAgainstVersion = 3.15f;

// The oldest API whose header was fetched and compiled to confirm that every
// field this driver touches is at the offset declared here. Older than this
// and the driver refuses rather than writing into memory whose shape it has
// not seen.
inline constexpr float kMinApiVersion = 3.07f;

// DeviceT::valid is padding before this; see the file header.
inline constexpr float kValidFieldSinceVersion = 3.08f;

// RxChannelParamsT::rspDxTunerParams sits at 140 from here and at 136 before
// 3.11; 3.12-3.14 were not checkable. The driver writes that member only at
// or above this version.
inline constexpr float kRspDxTunerLayoutVersion = 3.15f;

// A float compared for "at least" needs a tolerance, because 3.07 is not
// representable and `ver >= 3.07f` on a service that answers exactly 3.07 is a
// coin toss. The API's versions move in hundredths, so half of one is safe.
inline constexpr float kVersionEpsilon = 0.005f;

inline bool versionAtLeast(float have, float want) { return have > want - kVersionEpsilon; }

// --- constants ------------------------------------------------------------

inline constexpr unsigned kMaxDevices = 16;
inline constexpr std::size_t kMaxSerNoLen = 64;

// Hardware ids, sdrplay_api.h. RSP1B and RSPdx-R2 appear only at 3.15; the
// values are stable, and a device we do not know is shown by its number.
inline constexpr unsigned char kRsp1 = 1;
inline constexpr unsigned char kRsp2 = 2;
inline constexpr unsigned char kRspDuo = 3;
inline constexpr unsigned char kRspDx = 4;
inline constexpr unsigned char kRsp1B = 6;
inline constexpr unsigned char kRspDxR2 = 7;
inline constexpr unsigned char kRsp1A = 255;

// --- enums ----------------------------------------------------------------
//
// Each carries an explicit underlying type because the size is part of the
// ABI: ReasonForUpdateT has a 0x80000000 member, which makes it unsigned in
// C, and the rest are int. Four bytes either way, asserted below.

enum ErrT : int {
    Success = 0,
    Fail = 1,
    InvalidParam = 2,
    OutOfRange = 3,
    GainUpdateError = 4,
    RfUpdateError = 5,
    FsUpdateError = 6,
    HwError = 7,
    AliasingError = 8,
    AlreadyInitialised = 9,
    NotInitialised = 10,
    NotEnabled = 11,
    HwVerError = 12,
    OutOfMemError = 13,
    ServiceNotResponding = 14,
    StartPending = 15,
    StopPending = 16,
    InvalidMode = 17,
    FailedVerification1 = 18,
    FailedVerification2 = 19,
    FailedVerification3 = 20,
    FailedVerification4 = 21,
    FailedVerification5 = 22,
    FailedVerification6 = 23,
    InvalidServiceVersion = 24,
};

enum ReasonForUpdateT : unsigned int {
    Update_None = 0x00000000u,
    Update_Dev_Fs = 0x00000001u,
    Update_Dev_Ppm = 0x00000002u,
    Update_Dev_SyncUpdate = 0x00000004u,
    Update_Dev_ResetFlags = 0x00000008u,
    Update_Rsp1a_BiasTControl = 0x00000010u,
    Update_Rsp1a_RfNotchControl = 0x00000020u,
    Update_Rsp1a_RfDabNotchControl = 0x00000040u,
    Update_Rsp2_BiasTControl = 0x00000080u,
    Update_Rsp2_AmPortSelect = 0x00000100u,
    Update_Rsp2_AntennaControl = 0x00000200u,
    Update_Rsp2_RfNotchControl = 0x00000400u,
    Update_Rsp2_ExtRefControl = 0x00000800u,
    Update_RspDuo_ExtRefControl = 0x00001000u,
    Update_Tuner_Gr = 0x00008000u,
    Update_Tuner_GrLimits = 0x00010000u,
    Update_Tuner_Frf = 0x00020000u,
    Update_Tuner_BwType = 0x00040000u,
    Update_Tuner_IfType = 0x00080000u,
    Update_Tuner_DcOffset = 0x00100000u,
    Update_Tuner_LoMode = 0x00200000u,
    Update_Ctrl_DCoffsetIQimbalance = 0x00400000u,
    Update_Ctrl_Decimation = 0x00800000u,
    Update_Ctrl_Agc = 0x01000000u,
    Update_Ctrl_AdsbMode = 0x02000000u,
    Update_Ctrl_OverloadMsgAck = 0x04000000u,
    Update_RspDuo_BiasTControl = 0x08000000u,
    Update_RspDuo_AmPortSelect = 0x10000000u,
    Update_RspDuo_Tuner1AmNotchControl = 0x20000000u,
    Update_RspDuo_RfNotchControl = 0x40000000u,
    Update_RspDuo_RfDabNotchControl = 0x80000000u,
};

enum ReasonForUpdateExt1T : unsigned int {
    Update_Ext1_None = 0x00000000u,
    Update_RspDx_HdrEnable = 0x00000001u,
    Update_RspDx_BiasTControl = 0x00000002u,
    Update_RspDx_AntennaControl = 0x00000004u,
    Update_RspDx_RfNotchControl = 0x00000008u,
    Update_RspDx_RfDabNotchControl = 0x00000010u,
    Update_RspDx_HdrBw = 0x00000020u,
    Update_RspDuo_ResetSlaveFlags = 0x00000040u,
};

enum DbgLvlT : int {
    DbgLvl_Disable = 0,
    DbgLvl_Verbose = 1,
    DbgLvl_Warning = 2,
    DbgLvl_Error = 3,
    DbgLvl_Message = 4,
};

enum BwMHzT : int {
    BW_Undefined = 0,
    BW_0_200 = 200,
    BW_0_300 = 300,
    BW_0_600 = 600,
    BW_1_536 = 1536,
    BW_5_000 = 5000,
    BW_6_000 = 6000,
    BW_7_000 = 7000,
    BW_8_000 = 8000,
};

enum IfKHzT : int {
    IF_Undefined = -1,
    IF_Zero = 0,
    IF_0_450 = 450,
    IF_1_620 = 1620,
    IF_2_048 = 2048,
};

enum LoModeT : int {
    LO_Undefined = 0,
    LO_Auto = 1,
    LO_120MHz = 2,
    LO_144MHz = 3,
    LO_168MHz = 4,
};

enum MinGainReductionT : int {
    EXTENDED_MIN_GR = 0,
    NORMAL_MIN_GR = 20,
};

enum TunerSelectT : int {
    Tuner_Neither = 0,
    Tuner_A = 1,
    Tuner_B = 2,
    Tuner_Both = 3,
};

enum AgcControlT : int {
    AGC_DISABLE = 0,
    AGC_100HZ = 1,
    AGC_50HZ = 2,
    AGC_5HZ = 3,
    AGC_CTRL_EN = 4,
};

enum AdsbModeT : int {
    ADSB_DECIMATION = 0,
    ADSB_NO_DECIMATION_LOWPASS = 1,
    ADSB_NO_DECIMATION_BANDPASS_2MHZ = 2,
    ADSB_NO_DECIMATION_BANDPASS_3MHZ = 3,
};

enum TransferModeT : int {
    TRANSFER_ISOCH = 0,
    TRANSFER_BULK = 1,
};

enum Rsp2AntennaSelectT : int {
    Rsp2_ANTENNA_A = 5,
    Rsp2_ANTENNA_B = 6,
};

enum Rsp2AmPortSelectT : int {
    Rsp2_AMPORT_1 = 1,
    Rsp2_AMPORT_2 = 0,
};

enum RspDuoModeT : int {
    RspDuoMode_Unknown = 0,
    RspDuoMode_Single_Tuner = 1,
    RspDuoMode_Dual_Tuner = 2,
    RspDuoMode_Master = 4,
    RspDuoMode_Slave = 8,
};

enum RspDuoAmPortSelectT : int {
    RspDuo_AMPORT_1 = 1,
    RspDuo_AMPORT_2 = 0,
};

enum RspDxAntennaSelectT : int {
    RspDx_ANTENNA_A = 0,
    RspDx_ANTENNA_B = 1,
    RspDx_ANTENNA_C = 2,
};

enum RspDxHdrModeBwT : int {
    RspDx_HDRMODE_BW_0_200 = 0,
    RspDx_HDRMODE_BW_0_500 = 1,
    RspDx_HDRMODE_BW_1_200 = 2,
    RspDx_HDRMODE_BW_1_700 = 3,
};

enum PowerOverloadCbEventIdT : int {
    Overload_Detected = 0,
    Overload_Corrected = 1,
};

enum RspDuoModeCbEventIdT : int {
    MasterInitialised = 0,
    SlaveAttached = 1,
    SlaveDetached = 2,
    SlaveInitialised = 3,
    SlaveUninitialised = 4,
    MasterDllDisappeared = 5,
    SlaveDllDisappeared = 6,
};

enum EventT : int {
    GainChange = 0,
    PowerOverloadChange = 1,
    DeviceRemoved = 2,
    RspDuoModeChange = 3,
    // 3.15 only. An older service never sends it; nothing here depends on
    // the service knowing the value, only on us knowing what it means.
    DeviceFailure = 4,
};

static_assert(sizeof(ErrT) == 4, "ErrT is an int-sized enum in the vendor header");
static_assert(sizeof(ReasonForUpdateT) == 4, "ReasonForUpdateT is 32 bits");
static_assert(sizeof(ReasonForUpdateExt1T) == 4, "ReasonForUpdateExt1T is 32 bits");
static_assert(sizeof(TunerSelectT) == 4, "TunerSelectT is an int-sized enum");
static_assert(sizeof(IfKHzT) == 4, "IfKHzT is an int-sized enum");

// --- tuner parameters -----------------------------------------------------

struct GainValuesT {
    float curr;
    float max;
    float min;
};

struct GainT {
    int gRdB;                // IF gain REDUCTION in dB, 20..59
    unsigned char LNAstate;  // an index into the model's own LNA table
    unsigned char syncUpdate;
    MinGainReductionT minGr;
    GainValuesT gainVals;  // the service writes this; we only read it
};

struct RfFreqT {
    double rfHz;
    unsigned char syncUpdate;
};

struct DcOffsetTunerT {
    unsigned char dcCal;
    unsigned char speedUp;
    int trackTime;
    int refreshRateTime;
};

struct TunerParamsT {
    BwMHzT bwType;
    IfKHzT ifType;
    LoModeT loMode;
    GainT gain;
    RfFreqT rfFreq;
    DcOffsetTunerT dcOffsetTuner;
};

// --- control parameters ---------------------------------------------------

struct DcOffsetT {
    unsigned char DCenable;
    unsigned char IQenable;
};

struct DecimationT {
    unsigned char enable;
    unsigned char decimationFactor;
    unsigned char wideBandSignal;
};

struct AgcT {
    AgcControlT enable;
    int setPoint_dBfs;
    unsigned short attack_ms;
    unsigned short decay_ms;
    unsigned short decay_delay_ms;
    unsigned short decay_threshold_dB;
    int syncUpdate;
};

struct ControlParamsT {
    DcOffsetT dcOffset;
    DecimationT decimation;
    AgcT agc;
    AdsbModeT adsbMode;
};

// --- per-model parameters -------------------------------------------------

struct Rsp1aParamsT {
    unsigned char rfNotchEnable;
    unsigned char rfDabNotchEnable;
};

struct Rsp1aTunerParamsT {
    unsigned char biasTEnable;
};

struct Rsp2ParamsT {
    unsigned char extRefOutputEn;
};

struct Rsp2TunerParamsT {
    unsigned char biasTEnable;
    Rsp2AmPortSelectT amPortSel;
    Rsp2AntennaSelectT antennaSel;
    unsigned char rfNotchEnable;
};

struct RspDuoParamsT {
    int extRefOutputEn;
};

// Added after 3.11. Declared so the enclosing struct matches the 3.15 layout;
// never written by this driver, at any version - see the file header.
struct RspDuoResetSlaveFlagsT {
    unsigned char resetGainUpdate;
    unsigned char resetRfUpdate;
};

struct RspDuoTunerParamsT {
    unsigned char biasTEnable;
    RspDuoAmPortSelectT tuner1AmPortSel;
    unsigned char tuner1AmNotchEnable;
    unsigned char rfNotchEnable;
    unsigned char rfDabNotchEnable;
    RspDuoResetSlaveFlagsT resetSlaveFlags;
};

struct RspDxParamsT {
    unsigned char hdrEnable;
    unsigned char biasTEnable;
    RspDxAntennaSelectT antennaSel;
    unsigned char rfNotchEnable;
    unsigned char rfDabNotchEnable;
};

struct RspDxTunerParamsT {
    RspDxHdrModeBwT hdrBw;
};

// --- the two structures the service hands us pointers into ----------------

struct RxChannelParamsT {
    TunerParamsT tunerParams;
    ControlParamsT ctrlParams;
    Rsp1aTunerParamsT rsp1aTunerParams;
    Rsp2TunerParamsT rsp2TunerParams;
    RspDuoTunerParamsT rspDuoTunerParams;
    RspDxTunerParamsT rspDxTunerParams;
};

struct FsFreqT {
    double fsHz;
    unsigned char syncUpdate;
    unsigned char reCal;
};

struct SyncUpdateT {
    unsigned int sampleNum;
    unsigned int period;
};

struct ResetFlagsT {
    unsigned char resetGainUpdate;
    unsigned char resetRfUpdate;
    unsigned char resetFsUpdate;
};

struct DevParamsT {
    double ppm;
    FsFreqT fsFreq;
    SyncUpdateT syncUpdate;
    ResetFlagsT resetFlags;
    TransferModeT mode;
    unsigned int samplesPerPkt;  // the service writes this
    Rsp1aParamsT rsp1aParams;
    Rsp2ParamsT rsp2Params;
    RspDuoParamsT rspDuoParams;
    RspDxParamsT rspDxParams;
};

struct DeviceT {
    char SerNo[kMaxSerNoLen];
    unsigned char hwVer;
    TunerSelectT tuner;
    RspDuoModeT rspDuoMode;
    unsigned char valid;  // meaningless below 3.08 - see kValidFieldSinceVersion
    double rspDuoSampleFreq;
    void* dev;  // HANDLE in the vendor header
};

struct DeviceParamsT {
    DevParamsT* devParams;
    RxChannelParamsT* rxChannelA;
    RxChannelParamsT* rxChannelB;
};

struct ErrorInfoT {
    char file[256];
    char function[256];
    int line;
    char message[1024];
};

// --- callbacks ------------------------------------------------------------

struct GainCbParamT {
    unsigned int gRdB;
    unsigned int lnaGRdB;
    double currGain;
};

struct PowerOverloadCbParamT {
    PowerOverloadCbEventIdT powerOverloadChangeType;
};

struct RspDuoModeCbParamT {
    RspDuoModeCbEventIdT modeChangeType;
};

union EventParamsT {
    GainCbParamT gainParams;
    PowerOverloadCbParamT powerOverloadParams;
    RspDuoModeCbParamT rspDuoModeParams;
};

struct StreamCbParamsT {
    unsigned int firstSampleNum;
    int grChanged;
    int rfChanged;
    int fsChanged;
    unsigned int numSamples;
};

using StreamCallbackT = void (*)(short* xi, short* xq, StreamCbParamsT* params,
                                 unsigned int numSamples, unsigned int reset, void* cbContext);
using EventCallbackT = void (*)(EventT eventId, TunerSelectT tuner, EventParamsT* params,
                                void* cbContext);

struct CallbackFnsT {
    StreamCallbackT StreamACbFn;
    StreamCallbackT StreamBCbFn;
    EventCallbackT EventCbFn;
};

// --- the layout pins ------------------------------------------------------
//
// Every number below came out of MSVC compiling the VENDOR'S OWN headers for
// 3.07, 3.11 and 3.15 (x64, default packing). They agree on all of these; the
// two members where the versions differ are commented at the file head and
// handled by version gates in the driver, not by an assert.

static_assert(sizeof(GainValuesT) == 12 && alignof(GainValuesT) == 4, "GainValuesT layout");
static_assert(sizeof(GainT) == 24 && alignof(GainT) == 4, "GainT layout");
static_assert(sizeof(RfFreqT) == 16 && alignof(RfFreqT) == 8, "RfFreqT layout");
static_assert(sizeof(DcOffsetTunerT) == 12, "DcOffsetTunerT layout");
static_assert(sizeof(TunerParamsT) == 72 && alignof(TunerParamsT) == 8, "TunerParamsT layout");
static_assert(offsetof(TunerParamsT, gain) == 12, "TunerParamsT::gain offset");
static_assert(offsetof(TunerParamsT, rfFreq) == 40, "TunerParamsT::rfFreq offset");
static_assert(offsetof(TunerParamsT, dcOffsetTuner) == 56, "TunerParamsT::dcOffsetTuner offset");

static_assert(sizeof(DcOffsetT) == 2, "DcOffsetT layout");
static_assert(sizeof(DecimationT) == 3, "DecimationT layout");
static_assert(sizeof(AgcT) == 20 && alignof(AgcT) == 4, "AgcT layout");
static_assert(sizeof(ControlParamsT) == 32 && alignof(ControlParamsT) == 4, "ControlParamsT layout");
static_assert(offsetof(ControlParamsT, agc) == 8, "ControlParamsT::agc offset");
static_assert(offsetof(ControlParamsT, adsbMode) == 28, "ControlParamsT::adsbMode offset");

static_assert(sizeof(Rsp1aParamsT) == 2, "Rsp1aParamsT layout");
static_assert(sizeof(Rsp1aTunerParamsT) == 1, "Rsp1aTunerParamsT layout");
static_assert(sizeof(Rsp2ParamsT) == 1, "Rsp2ParamsT layout");
static_assert(sizeof(Rsp2TunerParamsT) == 16 && alignof(Rsp2TunerParamsT) == 4,
              "Rsp2TunerParamsT layout");
static_assert(sizeof(RspDuoParamsT) == 4, "RspDuoParamsT layout");
static_assert(sizeof(RspDuoResetSlaveFlagsT) == 2, "RspDuoResetSlaveFlagsT layout");
// 16 at 3.15; 12 at 3.07 and 3.11, where the last two members are padding in
// the service's own struct. Only ever grown into the enclosing struct's
// trailing padding, which is why RxChannelParamsT below is 144 in both.
static_assert(sizeof(RspDuoTunerParamsT) == 16 && alignof(RspDuoTunerParamsT) == 4,
              "RspDuoTunerParamsT layout (3.15)");
static_assert(sizeof(RspDxParamsT) == 12 && alignof(RspDxParamsT) == 4, "RspDxParamsT layout");
static_assert(sizeof(RspDxTunerParamsT) == 4, "RspDxTunerParamsT layout");

static_assert(sizeof(RxChannelParamsT) == 144 && alignof(RxChannelParamsT) == 8,
              "RxChannelParamsT layout - the same 144 bytes at 3.07, 3.11 and 3.15");
static_assert(offsetof(RxChannelParamsT, ctrlParams) == 72, "RxChannelParamsT::ctrlParams offset");
static_assert(offsetof(RxChannelParamsT, rsp1aTunerParams) == 104,
              "RxChannelParamsT::rsp1aTunerParams offset");
static_assert(offsetof(RxChannelParamsT, rsp2TunerParams) == 108,
              "RxChannelParamsT::rsp2TunerParams offset");
static_assert(offsetof(RxChannelParamsT, rspDuoTunerParams) == 124,
              "RxChannelParamsT::rspDuoTunerParams offset");
// 140 at 3.15, 136 at 3.07/3.11 - the ONE member whose offset moves, hence
// kRspDxTunerLayoutVersion and the driver's refusal to write it below 3.15.
static_assert(offsetof(RxChannelParamsT, rspDxTunerParams) == 140,
              "RxChannelParamsT::rspDxTunerParams offset (3.15)");

static_assert(sizeof(FsFreqT) == 16 && alignof(FsFreqT) == 8, "FsFreqT layout");
static_assert(sizeof(SyncUpdateT) == 8, "SyncUpdateT layout");
static_assert(sizeof(ResetFlagsT) == 3, "ResetFlagsT layout");
static_assert(sizeof(DevParamsT) == 64 && alignof(DevParamsT) == 8, "DevParamsT layout");
static_assert(offsetof(DevParamsT, fsFreq) == 8, "DevParamsT::fsFreq offset");
static_assert(offsetof(DevParamsT, mode) == 36, "DevParamsT::mode offset");
static_assert(offsetof(DevParamsT, samplesPerPkt) == 40, "DevParamsT::samplesPerPkt offset");
static_assert(offsetof(DevParamsT, rsp1aParams) == 44, "DevParamsT::rsp1aParams offset");
static_assert(offsetof(DevParamsT, rsp2Params) == 46, "DevParamsT::rsp2Params offset");
static_assert(offsetof(DevParamsT, rspDuoParams) == 48, "DevParamsT::rspDuoParams offset");
static_assert(offsetof(DevParamsT, rspDxParams) == 52, "DevParamsT::rspDxParams offset");

static_assert(sizeof(DeviceT) == 96 && alignof(DeviceT) == 8,
              "DeviceT layout - 96 bytes at 3.07 too, where `valid` is padding");
static_assert(offsetof(DeviceT, hwVer) == 64, "DeviceT::hwVer offset");
static_assert(offsetof(DeviceT, tuner) == 68, "DeviceT::tuner offset");
static_assert(offsetof(DeviceT, rspDuoMode) == 72, "DeviceT::rspDuoMode offset");
static_assert(offsetof(DeviceT, valid) == 76, "DeviceT::valid offset");
static_assert(offsetof(DeviceT, rspDuoSampleFreq) == 80, "DeviceT::rspDuoSampleFreq offset");
static_assert(offsetof(DeviceT, dev) == 88, "DeviceT::dev offset");

static_assert(sizeof(DeviceParamsT) == 24, "DeviceParamsT layout");
static_assert(sizeof(ErrorInfoT) == 1540, "ErrorInfoT layout");
static_assert(sizeof(StreamCbParamsT) == 20, "StreamCbParamsT layout");
static_assert(sizeof(GainCbParamT) == 16 && alignof(GainCbParamT) == 8, "GainCbParamT layout");
static_assert(sizeof(PowerOverloadCbParamT) == 4, "PowerOverloadCbParamT layout");
static_assert(sizeof(RspDuoModeCbParamT) == 4, "RspDuoModeCbParamT layout");
static_assert(sizeof(EventParamsT) == 16 && alignof(EventParamsT) == 8, "EventParamsT layout");
static_assert(sizeof(CallbackFnsT) == 24, "CallbackFnsT layout");

// --- the function table ---------------------------------------------------
//
// The seam. Every call the driver makes goes through one of these pointers,
// filled either by LoadLibrary/GetProcAddress against the user's install or,
// in tests, by a fake that answers the way the service does. There is no RSP
// and no SDRplay API on the machine this driver was written on, so the fake
// IS the proof, and the indirection that admits it is part of the design
// rather than a back door.
//
// The calling convention is the platform default on x64 Windows, which is
// what the vendor header's plain declarations use.

using OpenFn = ErrT (*)();
using CloseFn = ErrT (*)();
using ApiVersionFn = ErrT (*)(float* apiVer);
using LockDeviceApiFn = ErrT (*)();
using UnlockDeviceApiFn = ErrT (*)();
using GetDevicesFn = ErrT (*)(DeviceT* devices, unsigned int* numDevs, unsigned int maxDevs);
using SelectDeviceFn = ErrT (*)(DeviceT* device);
using ReleaseDeviceFn = ErrT (*)(DeviceT* device);
using GetErrorStringFn = const char* (*)(ErrT err);
using GetLastErrorFn = ErrorInfoT* (*)(DeviceT* device);
using DebugEnableFn = ErrT (*)(void* dev, DbgLvlT dbgLvl);
using GetDeviceParamsFn = ErrT (*)(void* dev, DeviceParamsT** deviceParams);
using InitFn = ErrT (*)(void* dev, CallbackFnsT* callbackFns, void* cbContext);
using UninitFn = ErrT (*)(void* dev);
using UpdateFn = ErrT (*)(void* dev, TunerSelectT tuner, ReasonForUpdateT reasonForUpdate,
                          ReasonForUpdateExt1T reasonForUpdateExt1);
using SwapRspDuoActiveTunerFn = ErrT (*)(void* dev, TunerSelectT* currentTuner,
                                         RspDuoAmPortSelectT tuner1AmPortSel);

// One process-wide connection to the service, refcounted.
//
// WHY THE SESSION LIVES IN THE TABLE. sdrplay_api_Open and _Close are
// PROCESS-scope: the second Open is not a second connection, and a Close made
// while another part of the application is enumerating pulls the floor out
// from under it. So the count of who wants the API open belongs to the table
// the calls go through, which is also what makes a test's fake table a
// genuinely separate world from the process one.
struct Api {
    Api() = default;
    // A mutex member makes this non-copyable, which is what we want: the
    // process table is a function-local static and a test's is a member of
    // the fake. A copied table would be a second, silently divergent count.
    Api(const Api&) = delete;
    Api& operator=(const Api&) = delete;

    // Filled by loadSdrPlayApi(); a test fills them by hand.
    bool resolved = false;      // the DLL was found and every entry point resolved
    std::string loadDetail;     // the path it came from, or why it did not
    OpenFn Open = nullptr;
    CloseFn Close = nullptr;
    ApiVersionFn ApiVersion = nullptr;
    LockDeviceApiFn LockDeviceApi = nullptr;
    UnlockDeviceApiFn UnlockDeviceApi = nullptr;
    GetDevicesFn GetDevices = nullptr;
    SelectDeviceFn SelectDevice = nullptr;
    ReleaseDeviceFn ReleaseDevice = nullptr;
    GetErrorStringFn GetErrorString = nullptr;
    GetLastErrorFn GetLastError = nullptr;
    DebugEnableFn DebugEnable = nullptr;
    GetDeviceParamsFn GetDeviceParams = nullptr;
    InitFn Init = nullptr;
    UninitFn Uninit = nullptr;
    UpdateFn Update = nullptr;
    SwapRspDuoActiveTunerFn SwapRspDuoActiveTuner = nullptr;

    // Session state, guarded by sessionMutex.
    mutable std::mutex sessionMutex;
    mutable int sessions = 0;
    mutable float version = 0.0f;  // what ApiVersion answered on the 0 -> 1 acquire
    // THE SESSION IS LOST FOR THE LIFE OF THE PROCESS. Set when a thread of
    // ours has been abandoned inside the vendor DLL or the service has
    // declared itself gone (sdrplay_api_ServiceNotResponding) - the moment the
    // driver stops entering the DLL for that DEVICE and orphans its session,
    // which can therefore never be closed. Once set, sessionAcquire refuses
    // and a scan returns without a vendor call: the 0.99.27 crash was a scan's
    // GetDevices through exactly such a session. Never cleared.
    mutable bool sessionLost = false;
};

}  // namespace cascade::source::sdrplay_abi
