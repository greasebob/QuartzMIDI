"""Transkun on ONNX Runtime: audio in, piano notes and pedals out.

The model is Transkun by Yujia Yan (MIT), its "2.0" checkpoint exported to
ONNX by Pianoscribe (setup.ps1 fetches it from Pianoscribe's publisher into
model\\). The feature frontend, the interval decoding and the segment stitching
below are adapted from Pianoscribe's piano_engine.py, Copyright (c) Justagwas,
GPL-3.0, https://github.com/Justagwas/Pianoscribe. On a 2.4 minute song they
give the notes Transkun's own PyTorch code gives, in a third of its time on
the CPU and a tenth on a DirectX 12 GPU through DirectML.
"""

import json
import math
import os
import subprocess
from collections import defaultdict
from dataclasses import dataclass

import numpy as np

MODEL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "model")
DML = "DmlExecutionProvider"
CPU = "CPUExecutionProvider"


@dataclass(slots=True)
class Event:
    start: float
    end: float
    pitch: int  # 21-108 for keys; -64 sustain and -67 soft pedal
    velocity: int
    has_onset: bool = True
    has_offset: bool = True


class Model:
    def __init__(self, device, threads):
        import onnxruntime as ort

        with open(os.path.join(MODEL, "manifest.json"), encoding="utf-8") as file:
            manifest = json.load(file)
        self.symbols = tuple(int(value) for value in manifest["symbols"])
        if len(self.symbols) != 90 or self.symbols[:2] != (-64, -67) or self.symbols[2:] != tuple(range(21, 109)):
            raise RuntimeError("The transcription model is not the one this converter reads.")
        self.sample_rate = int(manifest["sample_rate"])
        self.hop_size = int(manifest["hop_size"])
        self.window_size = int(manifest["window_size"])
        self.segment_seconds = float(manifest["segment_seconds"])
        self.segment_hop_seconds = float(manifest["segment_hop_seconds"])
        with np.load(os.path.join(MODEL, "frontend.npz"), allow_pickle=False) as frontend:
            self.windows = np.ascontiguousarray(frontend["windows"], dtype=np.float32)
            self.mel_filter = np.ascontiguousarray(frontend["mel_filter"], dtype=np.float32)
        self.threads = threads

        ort.set_default_logger_severity(3)
        wanted = [DML, CPU] if device != "cpu" and DML in ort.get_available_providers() else [CPU]
        self.scorer = self.attributes = None
        for providers in ([wanted, [CPU]] if wanted[0] == DML else [wanted]):
            options = ort.SessionOptions()
            options.intra_op_num_threads = threads
            options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
            if providers[0] == DML:
                # DirectML supports neither memory patterns nor parallel execution.
                options.enable_mem_pattern = False
                options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
            try:
                self.scorer = ort.InferenceSession(os.path.join(MODEL, "scorer.onnx"), options, providers=providers)
                self.attributes = ort.InferenceSession(os.path.join(MODEL, "attributes.onnx"), options, providers=providers)
                break
            except Exception as failure:
                if providers == [CPU]:
                    raise
                print(f"The GPU could not load the model ({failure}); using the CPU.", flush=True)
        self.gpu = self.scorer.get_providers()[0] == DML

    def features(self, segment):
        from scipy.fft import rfft

        samples = segment.shape[-1]
        frame_count = math.ceil(samples / self.hop_size) + 1
        left = self.window_size // 2
        right = (frame_count - 1) * self.hop_size + self.window_size - samples - left
        padded = np.pad(segment, ((0, 0), (left, max(0, right))))
        frames = np.lib.stride_tricks.as_strided(
            padded, shape=(padded.shape[0], frame_count, self.window_size),
            strides=(padded.strides[0], padded.strides[1] * self.hop_size, padded.strides[1]), writeable=False)
        normalized = (frames - np.mean(frames, dtype=np.float32)) / (np.std(frames, dtype=np.float32, ddof=1) + np.float32(1e-8))
        windows = self.windows.shape[0]
        mels = self.mel_filter.shape[1]
        result = np.zeros((1, frame_count, mels, windows), dtype=np.float32)
        for channel in normalized:
            spectrum = rfft(channel[:, None, :] * self.windows[None, :, :], axis=-1, norm="ortho", workers=self.threads)
            power = np.square(np.abs(spectrum), dtype=np.float32)
            projected = power.reshape(frame_count * windows, power.shape[-1]) @ self.mel_filter
            result[0] += projected.reshape(frame_count, windows, mels).transpose(0, 2, 1)
        result /= float(max(1, normalized.shape[0]))
        epsilon = np.float32(1e-5)
        result = (np.log(result + epsilon) - math.log(epsilon)) / (-math.log(epsilon))
        return np.ascontiguousarray(result, dtype=np.float32)


def decode(score, noise, forced_start):
    """Each track's best set of non-overlapping intervals (Transkun's semi-CRF),
    searched backwards from the segment's end."""
    steps, _, tracks = score.shape
    best = np.zeros((steps, tracks), dtype=np.float32)
    diagonal = score[np.arange(steps), np.arange(steps), :]
    best[-1] = diagonal[-1] * (diagonal[-1] > 0)
    pointers = np.empty((steps - 1, tracks), dtype=np.int32)
    for distance in range(1, steps):
        begin = steps - distance - 1
        candidates = np.concatenate((best[begin + 1:begin + 2] + noise[begin:begin + 1],
                                     best[begin + 1:] + score[begin + 1:, begin, :]), axis=0)
        selection = np.argmax(candidates, axis=0)
        best[begin] = candidates[selection, np.arange(tracks)] + diagonal[begin] * (diagonal[begin] > 0)
        pointers[distance - 1] = selection.astype(np.int32) - 1
    paths = []
    for track in range(tracks):
        position = max(0, min(int(forced_start[track]), steps - 1))
        intervals = []
        while position < steps - 1:
            selection = int(pointers[steps - position - 2, track])
            if diagonal[position, track] > 0:
                intervals.append((position, position))
            if selection < 0:
                position += 1
            else:
                end = selection + position + 1
                intervals.append((position, end))
                position = end
        if diagonal[-1, track] > 0:
            intervals.append((steps - 1, steps - 1))
        paths.append(intervals)
    return paths


def offset_fraction(logits):
    """The mean of a continuous Bernoulli, as a shift of -0.5 to 0.5 frames."""
    values = np.asarray(logits, dtype=np.float64)
    result = np.empty_like(values)
    small = np.abs(values) < 1e-3
    x = values[small]
    result[small] = 0.5 + x / 12.0 - x ** 3 / 720.0
    x = values[~small]
    result[~small] = 0.5 + 0.5 / np.tanh(x / 2.0) - 1.0 / x
    return np.clip((result - 0.5) / 0.99, -0.5, 0.5)


def segment_events(model, context, paths, frame_duration, last_frame):
    rows = [(track, start, end) for track, intervals in enumerate(paths) for start, end in intervals]
    if not rows:
        return [], [0] * len(model.symbols)
    velocity_logits, refined = model.attributes.run(
        ["velocity_logits", "refined_logits"],
        {"context": np.ascontiguousarray(context, dtype=np.float32),
         "interval_indices": np.asarray(rows, dtype=np.int64)})
    velocities = np.argmax(velocity_logits, axis=-1)
    refined = np.asarray(refined)
    offsets = offset_fraction(refined[:, :2])
    present = refined[:, 2:] > 0
    events, last_positions, row = [], [], 0
    for track, intervals in enumerate(paths):
        last_end, last_position = 0.0, 0
        for start_frame, end_frame in intervals:
            start = max((start_frame + offsets[row, 0]) * frame_duration, last_end)
            end = max((end_frame + offsets[row, 1]) * frame_duration, start + 1e-8)
            last_end = end
            event = Event(float(start), float(end), model.symbols[track], int(velocities[row]),
                          bool(start_frame > 0 or present[row, 0]), bool(end_frame < last_frame or present[row, 1]))
            events.append(event)
            if event.has_offset:
                last_position = end_frame
            row += 1
        last_positions.append(last_position)
    events.sort(key=lambda event: (event.start, event.end, event.pitch))
    return events, last_positions


def read_audio(path, sample_rate):
    """Samples as (channels, samples) float32 at the model's rate, decoded by FFmpeg."""
    result = subprocess.run(
        ["ffmpeg", "-nostdin", "-v", "error", "-i", path, "-vn", "-ac", "2", "-ar", str(sample_rate), "-f", "f32le", "-"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        raise RuntimeError("FFmpeg could not read the audio: " + result.stderr.decode("utf-8", "replace").strip())
    samples = np.frombuffer(result.stdout, dtype="<f4")
    samples = samples[:samples.size - samples.size % 2].reshape(-1, 2).T
    if samples.shape[1] == 0:
        raise RuntimeError("The audio has no samples.")
    return np.ascontiguousarray(np.nan_to_num(samples, nan=0.0, posinf=1.0, neginf=-1.0), dtype=np.float32)


def transcribe(path, device="auto", threads=None, progress=None):
    """The notes and pedals of the audio at path. progress(done, total, gpu) is
    called after each 8 second step."""
    threads = threads or os.cpu_count() or 1
    model = Model(device, threads)
    channels = read_audio(path, model.sample_rate)
    duration = channels.shape[1] / model.sample_rate

    padding_seconds = model.segment_seconds - model.segment_hop_seconds
    pad = math.ceil(padding_seconds * model.sample_rate)
    channels = np.pad(channels, ((0, 0), (pad, pad)))
    step = math.ceil(model.segment_hop_seconds * model.sample_rate / model.hop_size) * model.hop_size
    length = math.ceil(model.segment_seconds * model.sample_rate)
    starts = list(range(0, channels.shape[1], step))
    forced_start = [math.floor(padding_seconds * model.sample_rate / model.hop_size)] * len(model.symbols)
    frame_duration = model.hop_size / model.sample_rate
    last_frame = round(length / model.hop_size)
    by_symbol = defaultdict(list)
    batch = 2 if model.gpu else 1

    cursor = 0
    while cursor < len(starts):
        count = min(batch, len(starts) - cursor)
        features = []
        for start in starts[cursor:cursor + count]:
            segment = channels[:, start:start + length]
            if segment.shape[1] < length:
                segment = np.pad(segment, ((0, 0), (0, length - segment.shape[1])))
            features.append(model.features(segment))
        try:
            scores, noise, context = model.scorer.run(
                ["interval_scores", "skip_scores", "context"], {"mel_features": np.concatenate(features, axis=0)})
        except Exception:
            if count == 1:
                raise
            batch = 1  # a card with too little memory for two segments at once
            continue
        for index in range(count):
            paths = decode(scores[:, :, index, :], noise[:, index, :], forced_start)
            events, last_positions = segment_events(model, context[index:index + 1], paths, frame_duration, last_frame)
            forced_start = [max(position - step // model.hop_size, 0) for position in last_positions]
            begin = starts[cursor + index] / model.sample_rate - padding_seconds
            for event in events:
                event.start = max(0.0, event.start + begin)
                event.end = max(event.start, event.end + begin)
                found = by_symbol[event.pitch]
                if found and event.start < found[-1].end:
                    # The overlap of two segments: keep one copy of the note.
                    if event.has_onset:
                        found[-1] = event
                    else:
                        found[-1].has_offset = event.has_offset
                        found[-1].end = max(found[-1].end, event.end)
                elif event.has_onset:
                    found.append(event)
            if progress:
                progress(cursor + index + 1, len(starts), model.gpu)
        cursor += count

    for found in by_symbol.values():
        if found:
            found[-1].has_offset = True
    events = sorted((event for found in by_symbol.values() for event in found if event.has_offset),
                    key=lambda event: (event.start, event.end, event.pitch))
    # A key struck again ends the note it was holding.
    result, last = [], {}
    for event in events:
        if event.pitch in last and result[last[event.pitch]].end > event.start:
            result[last[event.pitch]].end = event.start
        last[event.pitch] = len(result)
        result.append(event)
    kept = []
    for event in result:
        event.end = min(duration, event.end)
        if event.start < event.end:
            kept.append(event)
    return kept


def write_midi(events, path):
    """One piano track at 960 ticks per beat and 120 bpm, so a tick is 1/1920 s."""
    import mido

    messages = []
    for event in events:
        if event.pitch > 0:
            velocity = max(1, min(127, event.velocity))
            messages.append((event.start, 1, mido.Message("note_on", note=event.pitch, velocity=velocity)))
            messages.append((event.end, 0, mido.Message("note_off", note=event.pitch, velocity=0)))
        else:
            control = -event.pitch
            messages.append((event.start, 1, mido.Message("control_change", control=control, value=max(64, min(127, event.velocity)))))
            messages.append((event.end, 0, mido.Message("control_change", control=control, value=0)))
    # Offs before ons at the same tick, so a struck-again key sounds.
    messages.sort(key=lambda item: (round(item[0] * 1920), item[1]))
    track = mido.MidiTrack([mido.Message("program_change", program=0, time=0)])
    tick = 0
    for seconds, _, message in messages:
        at = round(seconds * 1920)
        track.append(message.copy(time=at - tick))
        tick = at
    track.append(mido.MetaMessage("end_of_track", time=0))
    song = mido.MidiFile(ticks_per_beat=960)
    song.tracks.append(mido.MidiTrack([mido.MetaMessage("set_tempo", tempo=500000, time=0),
                                       mido.MetaMessage("end_of_track", time=0)]))
    song.tracks.append(track)
    song.save(path)
