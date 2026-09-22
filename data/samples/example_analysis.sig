# SignalScript stage 6 example: import, preprocess, analyze, plot, and export.
load "data/samples/example_signal.csv" as raw;
clean = raw |> normalize() |> lowpass(cutoff=100, order=63);
spec = clean |> fft(window="hann");
result = features(spec);
plot clean as "Filtered waveform";
plot spec as "One-sided amplitude spectrum";
save result to "example_signal_features.json";
