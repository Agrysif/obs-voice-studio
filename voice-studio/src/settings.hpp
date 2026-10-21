#pragma once

// Ключи настроек фильтра (так они хранятся в сцене OBS).

namespace vs::key {

constexpr const char *kMode = "mode";
constexpr const char *kPreset = "preset";
constexpr const char *kAppliedPreset = "applied_preset";
constexpr const char *kBypass = "bypass";
constexpr const char *kInputGain = "input_gain";

constexpr const char *kDenoiseEnabled = "denoise_enabled";
constexpr const char *kDenoiseAmount = "denoise_amount";

constexpr const char *kGateEnabled = "gate_enabled";
constexpr const char *kGateMode = "gate_mode";
constexpr const char *kGateThreshold = "gate_threshold";
constexpr const char *kGateRange = "gate_range";
constexpr const char *kGateAttack = "gate_attack";
constexpr const char *kGateHold = "gate_hold";
constexpr const char *kGateRelease = "gate_release";
constexpr const char *kGateVadSens = "gate_vad_sens";

constexpr const char *kEqEnabled = "eq_enabled";
constexpr const char *kHpfEnabled = "hpf_enabled";
constexpr const char *kHpfFreq = "hpf_freq";
constexpr const char *kHpfSlope = "hpf_slope";
constexpr const char *kLowGain = "eq_low_gain";
constexpr const char *kLowFreq = "eq_low_freq";
constexpr const char *kMudGain = "eq_mud_gain";
constexpr const char *kMudFreq = "eq_mud_freq";
constexpr const char *kMudQ = "eq_mud_q";
constexpr const char *kNasalGain = "eq_nasal_gain";
constexpr const char *kNasalFreq = "eq_nasal_freq";
constexpr const char *kNasalQ = "eq_nasal_q";
constexpr const char *kPresGain = "eq_pres_gain";
constexpr const char *kPresFreq = "eq_pres_freq";
constexpr const char *kPresQ = "eq_pres_q";
constexpr const char *kAirGain = "eq_air_gain";
constexpr const char *kAirFreq = "eq_air_freq";
constexpr const char *kLpfFreq = "lpf_freq";

constexpr const char *kDeessEnabled = "deess_enabled";
constexpr const char *kDeessAmount = "deess_amount";
constexpr const char *kDeessFreq = "deess_freq";

constexpr const char *kCompEnabled = "comp_enabled";
constexpr const char *kCompAmount = "comp_amount";
constexpr const char *kCompAttack = "comp_attack";
constexpr const char *kCompRelease = "comp_release";

constexpr const char *kWarmthEnabled = "warmth_enabled";
constexpr const char *kWarmthAmount = "warmth_amount";

constexpr const char *kPitchEnabled = "pitch_enabled";
constexpr const char *kPitchSemitones = "pitch_semitones";
constexpr const char *kFormantShift = "formant_shift";
constexpr const char *kFormantPreserve = "formant_preserve";
constexpr const char *kPitchQuality = "pitch_quality";

constexpr const char *kOutputGain = "output_gain";
constexpr const char *kLimiterEnabled = "limiter_enabled";
constexpr const char *kLimiterCeiling = "limiter_ceiling";

} // namespace vs::key
