"""Prepare selected local UT3 announcer audio and explicit UT4 lookup mappings.

UModel exports belong under RUN/Extracted. This creates PCM WAVs and a manifest;
it does not edit Unreal assets, build code, or cook a PAK.
"""
import argparse
import array
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import wave


STATUS = {
    "YouHaveTheFlag_02": "YouHaveTheFlag",
    "TheEnemyHasYourFlag": "TheEnemyHasYourFlag",
    "YouHaveTheFlagReturnToBase": "YouHaveTheFlagReturnToBase",
    "ThirtySecondsRemain": "30SecondsLeft",
    "1MinRemains": "1MinutesRemain",
    "2MinRemains": "2MinutesRemain",
    "3MinRemains": "3MinutesRemain",
    "FiveMinuteWarning": "5MinutesRemain",
    "Overtime": "Overtime",
    "SuddenDeath": "SuddenDeath",
    "WonMatch": "YouHaveWonTheMatch",
    "LostMatch": "YouHaveLostTheMatch",
    "FlawlessVictory": "FlawlessVictory",
    "HumiliatingDefeat": "HumiliatingDefeat",
    "FinalRound": "FinalRound",
    "NewRoundIn": "NewRoundIn",
    "DM_Kills10": "TenKillsRemain",
    "DM_Kills5": "FiveKillsRemain",
    "DM_Kills1": "OneKillRemains",
    "Play": "Play",
}
for team in ("Red", "Blue"):
    for action in ("Returned", "Dropped", "Taken"):
        STATUS[f"{team}Flag{action}"] = f"{team}Flag{action}"
    STATUS.update({
        f"{team}TeamScores": f"{team}TeamScores",
        f"{team}IncreasesLead": f"{team}TeamIncreasesTheirLead",
        f"{team}Dominating": f"{team}TeamDominating",
        f"{team}TeamTakesLead": f"{team}TeamTakesTheLead",
        f"{team}TeamIsTheWinner": f"{team}TeamWinsTheMatch",
        f"{team}TeamWinsTheRound": f"{team}TeamWinsTheRound",
        f"YouAreOn{team}Team": f"YouAreOnThe{team}Team",
    })
for number in range(1, 11):
    STATUS[f"CD{number}"] = f"Countdown_{number:02d}"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def normalized(name):
    return re.sub(r"[^a-z0-9]", "", name.lower())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--sox", type=Path, required=True)
    parser.add_argument("--content", type=Path, required=True)
    parser.add_argument("--ut3-cooked", type=Path, required=True)
    args = parser.parse_args()
    extracted = args.run / "Extracted"
    reward_files = sorted(extracted.glob("*/SoundNodeWave/A_RewardAnnouncer_*.ogg"))
    if len(reward_files) != 58:
        raise RuntimeError(f"Expected 40 base + 18 expansion reward waves, got {len(reward_files)}")
    status_dir = extracted / "A_Announcer_Status" / "SoundNodeWave"
    selected = [("Reward", path) for path in reward_files]
    selected += [("Status", status_dir / f"A_StatusAnnouncer_{name}.ogg")
                 for name in sorted(set(STATUS.values()))]
    root = "/Game/NetcodePlusOptional/Announcers/UT3"
    imports = []
    for category, source in selected:
        if not source.is_file():
            raise RuntimeError(f"Missing selected audio: {source}")
        name = "A_UT3_" + source.stem.split("Announcer_", 1)[1]
        destination = f"{root}/Audio/{category}"
        existing_asset = args.content / destination.removeprefix("/Game/") / (name + ".uasset")
        if existing_asset.exists():
            raise RuntimeError(f"Refusing to replace an existing asset: {existing_asset}")
        wav = args.run / "WAV" / category / (name + ".wav")
        wav.parent.mkdir(parents=True, exist_ok=True)
        if wav.exists():
            raise RuntimeError(f"Refusing to replace existing staged WAV: {wav}")
        result = subprocess.run([str(args.sox), "-G", str(source), "-b", "16", str(wav)],
                                capture_output=True, text=True, check=True)
        with wave.open(str(wav), "rb") as sound:
            channels, width, rate, frames = sound.getnchannels(), sound.getsampwidth(), sound.getframerate(), sound.getnframes()
            if width != 2 or channels not in (1, 2) or frames == 0:
                raise RuntimeError(f"Unexpected PCM format: {wav}")
            samples = array.array("h", sound.readframes(frames))
        peak = max(abs(value) for value in samples)
        rms = math.sqrt(sum(value * value for value in samples) / len(samples))
        if peak == 0:
            raise RuntimeError(f"Silent source: {source}")
        imports.append({
            "category": category, "original_name": source.stem, "source": str(wav),
            "source_ogg": str(source), "source_ogg_sha256": sha256(source),
            "wav_sha256": sha256(wav), "name": name, "destination": destination,
            "asset": f"{destination}/{name}.{name}", "channels": channels,
            "sample_rate": rate, "duration": frames / rate,
            "peak_dbfs": 20 * math.log10(peak / 32768),
            "rms_dbfs": 20 * math.log10(rms / 32768),
            "full_scale_samples": sum(abs(value) >= 32767 for value in samples),
            "conversion_log": result.stderr.strip(),
        })

    rewards = {row["original_name"].removeprefix("A_RewardAnnouncer_"): row["asset"]
               for row in imports if row["category"] == "Reward"}
    by_normalized = {normalized(name): asset for name, asset in rewards.items()}
    # Exact spoken-name aliases only: UT4 sometimes adds RW_/RZE_ or underscores.
    for path in sorted((args.content / "RestrictedAssets/Audio/AnnouncerReward").glob("A_Announcer_*.uasset")):
        key = path.stem.removeprefix("A_Announcer_")
        spoken = re.sub(r"^(RW_|RZE_)", "", key)
        match = by_normalized.get(normalized(spoken))
        if match and key.lower() not in {name.lower() for name in rewards}:
            rewards[key] = match

    status_by_original = {row["original_name"].removeprefix("A_StatusAnnouncer_"): row["asset"]
                          for row in imports if row["category"] == "Status"}
    statuses = {key: status_by_original[value] for key, value in STATUS.items()}
    # Preserve source names for future callers without inventing new event triggers.
    for name, asset in status_by_original.items():
        if name.lower() not in {key.lower() for key in statuses}:
            statuses[name] = asset

    packages = [args.ut3_cooked / "Sounds/INT/A_Announcer_Reward.upk",
                args.ut3_cooked / "Sounds/INT/A_Announcer_Status.upk",
                args.ut3_cooked / "UT3G/Sounds/INT/A_Announcer_UT3G.upk"]
    manifest = {
        "schema": 1, "pack_id": "UT3", "content_root": root,
        "announcer_asset": f"{root}/BP_NCPAnnouncer_UT3",
        "cook_anchor": "/Game/Blueprints/Netcode/MutAnnouncers",
        "source_packages": [{"path": str(path), "sha256": sha256(path)} for path in packages],
        "audio_conversion": "SoX -G, 16-bit PCM; original sample rate and channels; no normalization/effects",
        "imports": imports, "reward_mappings": rewards, "status_mappings": statuses,
        "stock_fallback": {"reward_path": "/Game/RestrictedAssets/Audio/AnnouncerReward/",
                           "reward_prefix": "A_Announcer_",
                           "status_path": "/Game/RestrictedAssets/Audio/AnnouncerStatus/",
                           "status_prefix": "A_AnnouncerF_"},
        "excluded": "Warfare/campaign status, expansion Leviathan lines and unidentified alternate recordings",
    }
    output = args.run / "import-manifest.json"
    output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"manifest": str(output), "waves": len(imports),
                      "reward_waves": len(reward_files), "status_waves": len(selected) - len(reward_files),
                      "reward_keys": len(rewards), "status_keys": len(statuses),
                      "conversion_warnings": sum(bool(row["conversion_log"]) for row in imports),
                      "full_scale_samples": sum(row["full_scale_samples"] for row in imports)}))


if __name__ == "__main__":
    main()
