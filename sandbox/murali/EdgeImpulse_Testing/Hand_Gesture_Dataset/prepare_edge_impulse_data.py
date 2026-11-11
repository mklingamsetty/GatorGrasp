# FILE: sandbox/murali/EdgeImpulse_Testing/Hand_Gesture_Dataset/prepare_edge_impulse_data.py
import os
import glob
import re
import argparse
from collections import defaultdict
import numpy as np
import pandas as pd

INPUT_DIR = "./raw_zenodo_data"
OUTPUT_DIR = "./edge_impulse_ready_csvs"
HEADER_OUT = "./test_vectors.h"

SAMPLE_RATE_HZ = 1000       # 1 kHz sampling rate
CSV_WINDOW_MS = 2000        # 2-second CSV chunks
EI_SLICE_MS = 300           # 300 ms inference window size for ESP32-S3 test header
SMOOTHING_WINDOW = 25       # 25 ms envelope smoothing
NUM_CHANNELS = 4            # 4 EMG channels (GIICA / MyoWare 2.0 array)

def filter_powerline_and_bandpass(signal, fs=1000):
    """
    Removes DC drift (<20 Hz) and 60 Hz power-line interference (plus harmonics)
    using FFT filtering before envelope rectification.
    """
    sig = np.array(signal, dtype=np.float64)
    sig = sig - np.mean(sig)
    freqs = np.fft.rfftfreq(len(sig), d=1.0 / fs)
    spec = np.fft.rfft(sig)

    # Bandpass 20 Hz - 450 Hz
    mask = (freqs >= 20.0) & (freqs <= 450.0)
    # Notch out 60 Hz power-line hum and harmonics (60, 120, 180, 240, 300, 360, 420 Hz)
    for harmonic in range(60, 480, 60):
        mask &= ~((freqs >= harmonic - 2.5) & (freqs <= harmonic + 2.5))

    return np.fft.irfft(spec * mask, n=len(sig))

def kaviles22_weighted_envelope(signal, window_size=25):
    """
    Filters 60 Hz hum, applies full-wave rectification, and computes
    trapezoidal window weighting over window_size samples at 1 kHz.
    """
    clean_ac = filter_powerline_and_bandpass(signal, fs=SAMPLE_RATE_HZ)
    rectified = np.abs(clean_ac)

    if window_size <= 1:
        return rectified

    weights = np.ones(window_size)
    ramp = window_size // 4
    if ramp > 0:
        weights[:ramp] = np.linspace(0.25, 1.0, ramp)
        weights[-ramp:] = np.linspace(1.0, 0.25, ramp)
    weights /= np.sum(weights)

    return np.convolve(rectified, weights, mode='same')

def clean_label(filepath):
    """Extract the gesture label from the dataset filename."""
    parent = os.path.basename(os.path.dirname(filepath)).lower()
    stem = os.path.splitext(os.path.basename(filepath))[0].lower()
    if re.fullmatch(r"\d+", stem):
        raw_label = f"gesture_{stem}"
    elif stem in ["data", "dataset", "emg", "signal"]:
        raw_label = parent
    else:
        raw_label = stem
    raw_label = re.sub(r'[^a-z0-9]+', '_', raw_label).strip('_')
    return raw_label if raw_label else "gesture"

def load_any_table(filepath, num_channels=4):
    """Loads CSV, TXT, or XLSX and extracts the EMG sensor columns."""
    ext = os.path.splitext(filepath)[1].lower()
    if ext == '.xlsx':
        df = pd.read_excel(filepath)
    else:
        df = pd.read_csv(filepath, sep=None, engine='python', header=None)

    df_num = df.apply(pd.to_numeric, errors='coerce').dropna(how='all', axis=1).dropna()

    if df_num.shape[1] >= num_channels + 1:
        col0_diff = np.diff(df_num.iloc[:100, 0].values)
        if np.all(col0_diff > 0) and np.std(col0_diff) < 1e-3:
            df_num = df_num.iloc[:, 1:]

    return df_num.iloc[:, :num_channels].values

def parse_args():
    parser = argparse.ArgumentParser(description="Prepare GIICA EMG recordings for Edge Impulse.")
    parser.add_argument("--input-dir", default=INPUT_DIR)
    parser.add_argument("--output-dir", default=OUTPUT_DIR)
    parser.add_argument("--header-out", default=HEADER_OUT)
    parser.add_argument("--test-subjects", default="", help="Comma-separated subject numbers (e.g. '7,8') for held-out testing.")
    parser.add_argument("--test-output-dir", default="", help="Output directory for held-out subjects.")
    parser.add_argument("--smoothing-window", type=int, default=SMOOTHING_WINDOW)
    parser.add_argument("--num-channels", type=int, default=NUM_CHANNELS, help="Number of EMG channels (3 or 4).")
    parser.add_argument("--norm-mode", choices=["global_scalar", "per_channel"], default="global_scalar",
                        help="Normalization mode: global_scalar keeps channel ratios intact; per_channel scales channels independently.")
    parser.add_argument("--add-diff-features", action="store_true", default=True,
                        help="Add emg_diff_23 and emg_ratio_23 virtual axes to distinctly separate Gesture 3 from Gesture 4.")
    parser.add_argument("--no-diff-features", dest="add_diff_features", action="store_false",
                        help="Disable virtual differential axes and keep only the raw EMG channels.")
    return parser.parse_args()

def main():
    args = parse_args()
    output_dir = os.path.abspath(args.output_dir)
    header_out = os.path.abspath(args.header_out)
    test_subjects = {
        v.strip().lower().replace("subject", "").strip()
        for v in args.test_subjects.split(",") if v.strip()
    }
    
    # If test subjects are requested without an explicit test directory, create a 'test' subfolder
    if test_subjects and not args.test_output_dir:
        train_output_dir = os.path.join(output_dir, "train")
        test_output_dir = os.path.join(output_dir, "test")
    elif args.test_output_dir:
        train_output_dir = output_dir
        test_output_dir = os.path.abspath(args.test_output_dir)
    else:
        train_output_dir = output_dir
        test_output_dir = ""

    os.makedirs(train_output_dir, exist_ok=True)
    for f in glob.glob(os.path.join(train_output_dir, "*.csv")):
        os.remove(f)
    if test_output_dir:
        os.makedirs(test_output_dir, exist_ok=True)
        for f in glob.glob(os.path.join(test_output_dir, "*.csv")):
            os.remove(f)

    all_files = []
    for ext in ("*.csv", "*.txt", "*.xlsx"):
        all_files.extend(glob.glob(os.path.join(args.input_dir, "**", ext), recursive=True))

    # Group files by subject so %MVC normalization is computed per-subject across ALL gestures
    subject_groups = defaultdict(list)
    for fpath in sorted(all_files):
        subject = os.path.basename(os.path.dirname(fpath)).lower()
        subject_number = subject.replace("subject", "").strip() or "0"
        subject_groups[subject_number].append(fpath)

    file_counter = 0
    saved_test_vectors = {}
    label_counts = defaultdict(int)

    print(f"[*] Processing {len(subject_groups)} subjects with norm_mode='{args.norm_mode}' | add_diff_features={args.add_diff_features}")

    for subject_number, fpaths in subject_groups.items():
        subject_recordings = []
        for fpath in fpaths:
            try:
                raw_data = load_any_table(fpath, args.num_channels)
                if raw_data.shape[0] < CSV_WINDOW_MS or raw_data.shape[1] < args.num_channels:
                    continue
                smoothed = np.column_stack([
                    kaviles22_weighted_envelope(raw_data[:, ch], args.smoothing_window)
                    for ch in range(args.num_channels)
                ])
                label = clean_label(fpath)
                subject_recordings.append((fpath, label, smoothed))
            except Exception as e:
                print(f"[!] Skipped {fpath}: {e}")

        if not subject_recordings:
            continue

        # Stack all subject data to compute subject-wide normalization
        all_subject_data = np.vstack([rec[2] for rec in subject_recordings])
        
        if args.norm_mode == "global_scalar":
            # Global scalar normalization: scales all channels by the SAME factor.
            # Preserves relative muscle activation ratios and prevents Gesture 3 from inverting into Gesture 4!
            subj_mins = float(np.percentile(all_subject_data, 2))
            subj_maxs = float(np.percentile(all_subject_data, 98))
            denom = max(subj_maxs - subj_mins, 1e-6)
        else:
            # Legacy per-channel normalization
            subj_mins = np.percentile(all_subject_data, 2, axis=0)
            subj_maxs = np.percentile(all_subject_data, 98, axis=0)
            denom = np.where((subj_maxs - subj_mins) < 1e-6, 1.0, (subj_maxs - subj_mins))

        is_test_subject = subject_number in test_subjects
        destination_dir = test_output_dir if (is_test_subject and test_output_dir) else train_output_dir

        for fpath, label, smoothed in subject_recordings:
            normed = np.clip((smoothed - subj_mins) / denom, 0.0, 1.0)
            label_counts[label] += 1

            num_windows = len(normed) // CSV_WINDOW_MS
            for w in range(num_windows):
                chunk = normed[w * CSV_WINDOW_MS : (w + 1) * CSV_WINDOW_MS]
                cols = {"timestamp": np.arange(0, CSV_WINDOW_MS, int(1000 / SAMPLE_RATE_HZ))}
                
                # Base EMG channels
                for ch in range(args.num_channels):
                    cols[f"emg{ch+1}"] = np.round(chunk[:, ch], 5)

                # Virtual differential channels (Ch2 = index 1, Ch3 = index 2)
                if args.add_diff_features and args.num_channels >= 3:
                    ch2 = chunk[:, 1]
                    ch3 = chunk[:, 2]
                    # Direct difference: positive for G3, negative for G4
                    diff_23 = ch2 - ch3
                    # Normalized ratio: bounds difference between -1.0 and +1.0
                    ratio_23 = (ch2 - ch3) / (ch2 + ch3 + 1e-4)
                    cols["emg_diff_23"] = np.round(diff_23, 5)
                    cols["emg_ratio_23"] = np.round(ratio_23, 5)

                ei_df = pd.DataFrame(cols)
                out_csv = os.path.join(
                    destination_dir,
                    f"{label}.subject_{subject_number}.sample_{file_counter:04d}.csv"
                )
                ei_df.to_csv(out_csv, index=False)
                file_counter += 1

                # Extract a 300 ms test slice from the center window for test_vectors.h
                if label not in saved_test_vectors and w == num_windows // 2:
                    vector_data = []
                    for ch in range(args.num_channels):
                        vector_data.append(chunk[500 : 500 + EI_SLICE_MS, ch])
                    if args.add_diff_features and args.num_channels >= 3:
                        vector_data.append(cols["emg_diff_23"][500 : 500 + EI_SLICE_MS])
                        vector_data.append(cols["emg_ratio_23"][500 : 500 + EI_SLICE_MS])
                    
                    # Interleave channels: [t0_ch0, t0_ch1, ..., t1_ch0, t1_ch1, ...]
                    interleaved = np.column_stack(vector_data).flatten()
                    saved_test_vectors[label] = interleaved

    num_channels_out = args.num_channels + (2 if (args.add_diff_features and args.num_channels >= 3) else 0)
    print(f"[SUCCESS] Generated {file_counter} CSVs across classes: {dict(label_counts)}")
    print(f"          Output columns: {list(cols.keys())[1:]} ({num_channels_out} channels total)")
    print(f"          Destination: {train_output_dir}" + (f" and {test_output_dir}" if test_output_dir else ""))

    with open(header_out, "w") as f:
        f.write(f"// Auto-generated real EMG test vectors ({EI_SLICE_MS}ms x {num_channels_out} channels)\n#pragma once\n\n")
        for lbl, vec in sorted(saved_test_vectors.items()):
            c_name = re.sub(r'[^a-zA-Z0-9_]', '_', lbl)
            vals = ", ".join([f"{v:.4f}f" for v in vec])
            f.write(f"const float test_{c_name}[{len(vec)}] = {{ {vals} }};\n\n")
    print(f"[*] Updated C++ header test vectors: {header_out} ({EI_SLICE_MS}ms x {num_channels_out} channels = {EI_SLICE_MS * num_channels_out} floats)")

if __name__ == "__main__":
    main()
