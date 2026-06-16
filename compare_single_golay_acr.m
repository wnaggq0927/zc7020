clc;
clear;
close all;

set(groot, 'defaultAxesFontName', 'Microsoft YaHei');
set(groot, 'defaultTextFontName', 'Microsoft YaHei');
set(groot, 'defaultLegendFontName', 'Microsoft YaHei');

%% Input data
T_single = readtable('single_pulse_20us.csv');
T_golay = readtable('golay_80ns_gx_250mhz.csv');

dist_s = T_single.distance_m;
single_raw = T_single.single_raw;

dist_g = T_golay.distance_m;
ap = T_golay.ap;
an = T_golay.an;
bp = T_golay.bp;
bn = T_golay.bn;

%% Single-pulse baseline removal
tail_len = min(5000, floor(length(single_raw) / 10));
tail_start = max(1, length(single_raw) - tail_len + 1);
baseline = median(single_raw(tail_start:end));

head_len = min(tail_len, length(single_raw));
head = single_raw(1:head_len);
if abs(prctile(head, 95) - baseline) < abs(baseline - prctile(head, 5))
    single_clean = baseline - single_raw;
else
    single_clean = single_raw - baseline;
end
single_clean = max(single_clean, 0);

%% Generate the 256-bit Golay pair
a = [1 1];
b = [1 -1];
N = 8;
while N > 1
    olda = a;
    oldb = b;
    a = [olda oldb];
    b = [olda -oldb];
    N = N - 1;
end

seq_ap_base = a;
seq_bp_base = b;

pulse_width_ns = 80;
sample_rate_msps = 250;
sps_golay = round(pulse_width_ns * 1e-9 * sample_rate_msps * 1e6);

% Keep this setting identical to the reference ACR script.
use_pulse_filter = false;

ap_up = upsample(seq_ap_base, sps_golay);
bp_up = upsample(seq_bp_base, sps_golay);

if use_pulse_filter
    h_filter = rcosdesign(1, 6, sps_golay, 'normal');
else
    h_filter = ones(1, sps_golay);
end
h_filter = h_filter / max(h_filter);

seq_ap = conv(ap_up, h_filter, 'same');
seq_bp = conv(bp_up, h_filter, 'same');
seq_ap = seq_ap(:);
seq_bp = seq_bp(:);

%% Zeroth-order Golay matched correlation
diff_A = ap - an;
diff_B = bp - bn;

corr_diff_A = conv(diff_A, flip(seq_ap), 'same');
corr_diff_B = conv(diff_B, flip(seq_bp), 'same');
golay_0st = corr_diff_A + corr_diff_B;

%% First-order ACR, identical to the reference implementation
r_current = golay_0st;
N_len = length(r_current);

[~, maxidx] = max(abs(r_current));
r_current_shifted = circshift(r_current, -(maxidx - 1));

max_order = 1;
chip_width = sps_golay;
centre_ratio = ceil(chip_width / 1.85);
half_window = length(seq_ap);

mask = zeros(N_len, 1);
idx_front = 1:half_window;
idx_back = (N_len - half_window + 1):N_len;
idx_front = idx_front(idx_front <= N_len);
idx_back = idx_back(idx_back > 0);
mask(idx_front) = 1;
mask(idx_back) = 1;

r_history = cell(1, max_order + 1);
r_history{1} = r_current;

for order = 1:max_order
    r_remain = -r_current_shifted;
    r_remain = r_remain .* mask;

    peak_abs = abs(r_remain(1));
    if peak_abs <= eps
        error('ACR reference peak is zero; the suppression kernel cannot be normalized.');
    end
    r_remain = r_remain / peak_abs;

    r_remain(1:centre_ratio) = -r_remain(1:centre_ratio);
    r_remain(end - centre_ratio + 1:end) = ...
        -r_remain(end - centre_ratio + 1:end);

    r_current_shifted = ifft(fft(r_current_shifted) .* fft(r_remain));
    if isreal(golay_0st)
        r_current_shifted = real(r_current_shifted);
    end

    r_history{order + 1} = circshift(r_current_shifted, maxidx - 1);
end

golay_1st = r_history{2};

%% Normalize each curve independently and convert to power dB
floor_val = 1e-8;

single_peak = max(abs(single_clean));
golay_0_peak = max(abs(golay_0st));
golay_1_peak = max(abs(golay_1st));

if single_peak <= eps
    single_peak = 1;
end
if golay_0_peak <= eps
    golay_0_peak = 1;
end
if golay_1_peak <= eps
    golay_1_peak = 1;
end

single_db = 10 * log10(max(abs(single_clean) / single_peak, floor_val));
golay_0_db = 10 * log10(max(abs(golay_0st) / golay_0_peak, floor_val));
golay_1_db = 10 * log10(max(abs(golay_1st) / golay_1_peak, floor_val));

%% Plot comparison
figure( ...
    'Name', 'Single pulse vs Golay zeroth-order and first-order ACR', ...
    'Position', [150, 150, 1200, 600]);

step_s = max(1, ceil(length(dist_s) / 65000));
step_g = max(1, ceil(length(dist_g) / 65000));

plot(dist_s(1:step_s:end), single_db(1:step_s:end), ...
    'b', 'LineWidth', 1.0);
hold on;
plot(dist_g(1:step_g:end), golay_0_db(1:step_g:end), ...
    'Color', [0.85, 0.32, 0.09], 'LineWidth', 1.2);
plot(dist_g(1:step_g:end), golay_1_db(1:step_g:end), ...
    'Color', [0.46, 0.67, 0.18], 'LineWidth', 1.5);

title('20 us single pulse vs 80 ns 256-bit Golay');
xlabel('Distance (m)');
ylabel('Normalized amplitude (dB)');
xlim([0, max([dist_s(:); dist_g(:)])]);
ylim([-80, 5]);
grid on;
legend( ...
    'Single pulse 20 us', ...
    'Golay zeroth-order matched correlation', ...
    'Golay first-order ACR', ...
    'Location', 'northeast');

noise_count_s = min(1000, length(single_db));
noise_count_g = min(1000, length(golay_0_db));
fprintf('Single-pulse tail mean: %.2f dB\n', ...
    mean(single_db(end - noise_count_s + 1:end)));
fprintf('Golay zeroth-order tail mean: %.2f dB\n', ...
    mean(golay_0_db(end - noise_count_g + 1:end)));
fprintf('Golay first-order tail mean: %.2f dB\n', ...
    mean(golay_1_db(end - noise_count_g + 1:end)));
