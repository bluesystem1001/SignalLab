# Repeatable live-demo script: it deliberately does not save a file.
load "data/samples/example_signal.csv" as raw;
clean = raw |> normalize() |> lowpass(cutoff=100, order=63);
spec = clean |> fft(window="hann");
result = features(spec);
plot clean as "Filtered waveform";
plot spec as "One-sided amplitude spectrum";

