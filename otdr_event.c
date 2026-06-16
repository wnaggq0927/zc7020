#include "otdr_event.h"

#include "otdr_config.h"

#include "xil_printf.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define OTDR_EVENT_BINS 8192U
#define OTDR_EVENT_UNUSED 8192.0f
#define OTDR_LIGHT_SPEED_M_S 299792458.0f

typedef struct {
    uint32_t sample;
    uint32_t type;
    float score;
    float reflect_db;
    float loss_db;
} event_candidate_t;

static float event_db[OTDR_EVENT_BINS];
static float event_smooth[OTDR_EVENT_BINS];
static event_candidate_t event_candidates[OTDR_MAX_EVENTS * 4U];

static float mean_range(const float *data, uint32_t begin, uint32_t end)
{
    if (end <= begin) {
        return 0.0f;
    }

    double sum = 0.0;
    for (uint32_t i = begin; i < end; ++i) {
        sum += data[i];
    }
    return (float)(sum / (double)(end - begin));
}

static void mean_std_range(const float *data,
                           uint32_t begin,
                           uint32_t end,
                           float *mean,
                           float *stddev)
{
    *mean = mean_range(data, begin, end);
    if (end <= begin + 1U) {
        *stddev = 0.0f;
        return;
    }

    double variance = 0.0;
    for (uint32_t i = begin; i < end; ++i) {
        const double delta = (double)data[i] - (double)*mean;
        variance += delta * delta;
    }
    *stddev = (float)sqrt(variance / (double)(end - begin));
}

static float low_fraction(const float *data,
                          uint32_t begin,
                          uint32_t end,
                          float threshold)
{
    if (end <= begin) {
        return 0.0f;
    }

    uint32_t low_count = 0U;
    for (uint32_t i = begin; i < end; ++i) {
        if (data[i] <= threshold) {
            ++low_count;
        }
    }
    return (float)low_count / (float)(end - begin);
}

static float max_range(const float *data, uint32_t begin, uint32_t end)
{
    if (end <= begin) {
        return -1000.0f;
    }

    float maximum = data[begin];
    for (uint32_t i = begin + 1U; i < end; ++i) {
        if (data[i] > maximum) {
            maximum = data[i];
        }
    }
    return maximum;
}

static float raw_db_mean(const float *curve,
                         uint32_t begin,
                         uint32_t end,
                         float baseline,
                         float reference)
{
    if (end <= begin) {
        return -80.0f;
    }

    double sum = 0.0;
    const float floor = reference * 1.0e-8f;
    for (uint32_t i = begin; i < end; ++i) {
        float amplitude = fabsf(curve[i] - baseline);
        if (amplitude < floor) {
            amplitude = floor;
        }
        sum += 10.0 * log10((double)amplitude / (double)reference);
    }
    return (float)(sum / (double)(end - begin));
}

static uint32_t refine_loss_sample(const float *curve,
                                   uint32_t point_count,
                                   uint32_t coarse_sample,
                                   uint32_t search_radius,
                                   uint32_t edge_window,
                                   float baseline,
                                   float reference)
{
    if (edge_window < 2U) {
        edge_window = 2U;
    }

    uint32_t search_begin =
        (coarse_sample > search_radius)
            ? coarse_sample - search_radius
            : edge_window;
    if (search_begin < edge_window) {
        search_begin = edge_window;
    }

    uint32_t search_end = coarse_sample + search_radius;
    if (search_end + edge_window >= point_count) {
        search_end = point_count - edge_window - 1U;
    }
    if (search_end <= search_begin) {
        return coarse_sample;
    }

    uint32_t best_sample = coarse_sample;
    float best_drop = -1000.0f;
    for (uint32_t sample = search_begin;
         sample <= search_end;
         ++sample) {
        const float before =
            raw_db_mean(curve,
                        sample - edge_window,
                        sample,
                        baseline,
                        reference);
        const float after =
            raw_db_mean(curve,
                        sample,
                        sample + edge_window,
                        baseline,
                        reference);
        const float drop = before - after;
        if (drop > best_drop) {
            best_drop = drop;
            best_sample = sample;
        }
    }
    return best_sample;
}

static float linear_slope(const float *data, uint32_t begin, uint32_t end)
{
    if (end <= begin + 2U) {
        return 0.0f;
    }

    const double count = (double)(end - begin);
    double sx = 0.0;
    double sy = 0.0;
    double sxx = 0.0;
    double sxy = 0.0;
    for (uint32_t i = begin; i < end; ++i) {
        const double x = (double)i;
        const double y = (double)data[i];
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }

    const double denominator = count * sxx - sx * sx;
    if (fabs(denominator) < 1.0e-12) {
        return 0.0f;
    }
    return (float)((count * sxy - sx * sy) / denominator);
}

static void add_candidate(uint32_t *count,
                          uint32_t sample,
                          uint32_t type,
                          float score,
                          float reflect_db,
                          float loss_db)
{
    const uint32_t capacity =
        (uint32_t)(sizeof(event_candidates) / sizeof(event_candidates[0]));
    if (*count >= capacity) {
        return;
    }

    event_candidate_t *candidate = &event_candidates[*count];
    candidate->sample = sample;
    candidate->type = type;
    candidate->score = score;
    candidate->reflect_db = reflect_db;
    candidate->loss_db = loss_db;
    ++(*count);
}

static void sort_candidates(uint32_t count)
{
    for (uint32_t i = 1U; i < count; ++i) {
        event_candidate_t value = event_candidates[i];
        uint32_t j = i;
        while (j > 0U &&
               event_candidates[j - 1U].sample > value.sample) {
            event_candidates[j] = event_candidates[j - 1U];
            --j;
        }
        event_candidates[j] = value;
    }
}

static uint32_t merge_candidates(uint32_t count, uint32_t minimum_gap)
{
    if (count == 0U) {
        return 0U;
    }

    sort_candidates(count);
    uint32_t output_count = 1U;
    for (uint32_t i = 1U; i < count; ++i) {
        event_candidate_t *previous =
            &event_candidates[output_count - 1U];
        event_candidate_t *current = &event_candidates[i];
        if (current->sample - previous->sample >= minimum_gap) {
            event_candidates[output_count++] = *current;
        } else if (current->score > previous->score) {
            *previous = *current;
        }
    }
    return output_count;
}

uint32_t otdr_event_analyze(const float *curve,
                            uint32_t point_count,
                            otdr_event_point_t *events,
                            uint32_t event_capacity)
{
    if (curve == NULL || events == NULL ||
        point_count < 512U || event_capacity == 0U) {
        return 0U;
    }

    const otdr_config_t *config = otdr_config_get();
    const float sample_rate =
        config->downsample_enable ? 125000000.0f : 250000000.0f;
    const float sample_period_ns = 1.0e9f / sample_rate;
    uint32_t sps =
        (uint32_t)(config->pulse_width_ns / sample_period_ns + 0.5f);
    if (sps == 0U) {
        sps = 1U;
    }

    uint32_t block_size =
        (point_count + OTDR_EVENT_BINS - 1U) / OTDR_EVENT_BINS;
    if (block_size < 2U) {
        block_size = 2U;
    }
    uint32_t bin_count = point_count / block_size;
    if (bin_count > OTDR_EVENT_BINS) {
        bin_count = OTDR_EVENT_BINS;
    }
    if (bin_count < 128U) {
        return 0U;
    }

    const uint32_t tail_points =
        (point_count / 50U > 256U) ? point_count / 50U : 256U;
    const uint32_t tail_start =
        (tail_points < point_count) ? point_count - tail_points : 0U;
    const float baseline = mean_range(curve, tail_start, point_count);

    float reference = 0.0f;
    for (uint32_t i = 0U; i < point_count; ++i) {
        const float amplitude = fabsf(curve[i] - baseline);
        if (amplitude > reference) {
            reference = amplitude;
        }
    }
    if (reference < 1.0e-9f) {
        return 0U;
    }

    for (uint32_t bin = 0U; bin < bin_count; ++bin) {
        const uint32_t begin = bin * block_size;
        uint32_t end = begin + block_size;
        if (end > point_count) {
            end = point_count;
        }

        double amplitude_sum = 0.0;
        for (uint32_t i = begin; i < end; ++i) {
            amplitude_sum += fabs((double)curve[i] - (double)baseline);
        }
        float amplitude =
            (float)(amplitude_sum / (double)(end - begin));
        if (amplitude < reference * 1.0e-8f) {
            amplitude = reference * 1.0e-8f;
        }
        event_db[bin] = 10.0f * log10f(amplitude / reference);
    }

    for (uint32_t bin = 0U; bin < bin_count; ++bin) {
        const uint32_t begin = (bin > 2U) ? bin - 2U : 0U;
        uint32_t end = bin + 3U;
        if (end > bin_count) {
            end = bin_count;
        }
        event_smooth[bin] = mean_range(event_db, begin, end);
    }

    const uint32_t noise_begin = bin_count - bin_count / 10U;
    float noise_mean = 0.0f;
    float noise_std = 0.0f;
    mean_std_range(event_smooth, noise_begin, bin_count,
                   &noise_mean, &noise_std);

    uint32_t event_window =
        (sps * 12U + block_size - 1U) / block_size;
    if (event_window < 6U) {
        event_window = 6U;
    }
    if (event_window > 128U) {
        event_window = 128U;
    }

    uint32_t analysis_start =
        (config->wave_type == 1U)
            ? event_window * 2U
            : (sps * 256U + block_size - 1U) / block_size;
    if (analysis_start < event_window * 2U) {
        analysis_start = event_window * 2U;
    }
    if (analysis_start > bin_count / 3U) {
        analysis_start = bin_count / 3U;
    }

    const float noise_margin =
        fmaxf(config->end_threshold_db,
              2.5f * noise_std + 2.0f);
    const float noise_threshold = noise_mean + noise_margin;
    uint32_t noise_boundary = bin_count;
    const uint32_t persistence = event_window * 2U;
    uint32_t signal_lookback =
        (config->wave_type == 1U)
            ? event_window * 6U
            : (sps * 256U + block_size - 1U) / block_size;
    if (signal_lookback < persistence * 2U) {
        signal_lookback = persistence * 2U;
    }

    for (uint32_t bin = analysis_start;
         bin + persistence < noise_begin;
         ++bin) {
        const uint32_t lookback_begin =
            (bin > signal_lookback) ? bin - signal_lookback : analysis_start;
        const float previous_signal =
            max_range(event_smooth, lookback_begin, bin);
        const float after =
            mean_range(event_smooth, bin, bin + persistence);
        const float noise_ratio =
            low_fraction(event_smooth,
                         bin,
                         bin + persistence,
                         noise_threshold + 1.0f);
        if (previous_signal > noise_threshold + 3.0f &&
            after <= noise_threshold + 0.5f &&
            noise_ratio >= 0.75f) {
            noise_boundary = bin;
            break;
        }
    }

    if (noise_boundary == bin_count) {
        uint32_t bin = noise_begin;
        while (bin > analysis_start + persistence) {
            const uint32_t begin = bin - persistence;
            const float level =
                mean_range(event_smooth, begin, bin);
            if (level > noise_threshold + 2.0f) {
                noise_boundary = bin;
                break;
            }
            bin -= event_window;
        }
    }

    uint32_t analysis_end = noise_boundary;
    if (analysis_end < bin_count) {
        uint32_t guard = event_window * 2U;
        if (analysis_end > guard) {
            analysis_end -= guard;
        }
    }

    uint32_t candidate_count = 0U;
    const float reflection_threshold =
        fmaxf(3.0f, config->end_threshold_db);
    for (uint32_t bin = analysis_start + event_window;
         bin + event_window < analysis_end;
         ++bin) {
        const float left =
            mean_range(event_smooth, bin - event_window, bin);
        const float right =
            mean_range(event_smooth, bin + 1U, bin + event_window + 1U);
        const float prominence =
            event_smooth[bin] - fmaxf(left, right);
        if (event_smooth[bin] >= event_smooth[bin - 1U] &&
            event_smooth[bin] > event_smooth[bin + 1U] &&
            prominence >= reflection_threshold) {
            add_candidate(&candidate_count,
                          bin * block_size,
                          1U,
                          prominence,
                          prominence,
                          OTDR_EVENT_UNUSED);
        }
    }

    const float loss_threshold =
        (config->nonreflect_threshold_db > 0.0f)
            ? config->nonreflect_threshold_db
            : 0.35f;
    for (uint32_t bin = analysis_start + event_window * 2U;
         bin + event_window * 2U < analysis_end;
         bin += event_window / 2U) {
        float left_mean = 0.0f;
        float left_std = 0.0f;
        float right_mean = 0.0f;
        float right_std = 0.0f;
        mean_std_range(event_db,
                       bin - event_window * 2U,
                       bin - event_window,
                       &left_mean,
                       &left_std);
        mean_std_range(event_db,
                       bin + event_window,
                       bin + event_window * 2U,
                       &right_mean,
                       &right_std);

        const float loss = left_mean - right_mean;
        if (left_mean <= noise_threshold + 4.0f ||
            right_mean <= noise_threshold + 3.0f) {
            continue;
        }
        const float platform_noise = fmaxf(left_std, right_std);
        const float transition_noise =
            0.5f * (fabsf(event_smooth[bin] - left_mean) +
                    fabsf(event_smooth[bin + event_window] - right_mean));
        const float local_noise =
            fmaxf(0.05f, fmaxf(platform_noise, transition_noise));
        const float adaptive_loss_threshold =
            fmaxf(loss_threshold, 4.0f * platform_noise);

        if (loss >= adaptive_loss_threshold && loss <= 8.0f) {
            const float left_slope =
                linear_slope(event_smooth,
                             bin - event_window * 2U,
                             bin - event_window);
            const float right_slope =
                linear_slope(event_smooth,
                             bin + event_window,
                             bin + event_window * 2U);
            const float slope_change =
                fabsf(left_slope - right_slope) * (float)event_window;

            if (loss >= local_noise * 3.0f &&
                platform_noise <= 0.30f &&
                slope_change <= fmaxf(0.20f, loss * 0.75f)) {
                add_candidate(&candidate_count,
                              bin * block_size,
                              2U,
                              loss / local_noise,
                              OTDR_EVENT_UNUSED,
                              loss);
            }
        }
    }

    uint32_t minimum_gap =
        (config->wave_type == 1U) ? sps * 12U : sps * 256U;
    if (minimum_gap < block_size * 4U) {
        minimum_gap = block_size * 4U;
    }
    candidate_count = merge_candidates(candidate_count, minimum_gap);

    const uint32_t loss_search_radius =
        event_window * block_size * 2U;
    uint32_t loss_edge_window = sps * 2U;
    if (loss_edge_window < block_size) {
        loss_edge_window = block_size;
    }
    for (uint32_t i = 0U; i < candidate_count; ++i) {
        if (event_candidates[i].type != 2U) {
            continue;
        }
        event_candidates[i].sample =
            refine_loss_sample(curve,
                               point_count,
                               event_candidates[i].sample,
                               loss_search_radius,
                               loss_edge_window,
                               baseline,
                               reference);
    }
    sort_candidates(candidate_count);

    if (noise_boundary < bin_count) {
        uint32_t search_bins =
            (config->wave_type == 1U)
                ? event_window * 4U
                : (sps * 256U + block_size - 1U) / block_size;
        if (search_bins < event_window * 2U) {
            search_bins = event_window * 2U;
        }
        const uint32_t search_begin =
            (noise_boundary > search_bins)
                ? noise_boundary - search_bins
                : analysis_start;
        uint32_t search_end = noise_boundary + search_bins;
        if (search_end > bin_count) {
            search_end = bin_count;
        }

        uint32_t peak_bin = noise_boundary;
        float peak_value = -1000.0f;
        for (uint32_t bin = search_begin; bin < search_end; ++bin) {
            if (event_smooth[bin] > peak_value) {
                peak_value = event_smooth[bin];
                peak_bin = bin;
            }
        }

        uint32_t peak_sample = peak_bin * block_size;
        uint32_t refine_begin = peak_sample;
        if (refine_begin > block_size * 2U) {
            refine_begin -= block_size * 2U;
        } else {
            refine_begin = 0U;
        }
        uint32_t refine_end = peak_sample + block_size * 3U;
        if (refine_end > point_count) {
            refine_end = point_count;
        }
        float peak_amplitude = -1.0f;
        for (uint32_t sample = refine_begin;
             sample < refine_end;
             ++sample) {
            const float amplitude = fabsf(curve[sample] - baseline);
            if (amplitude > peak_amplitude) {
                peak_amplitude = amplitude;
                peak_sample = sample;
            }
        }

        candidate_count = merge_candidates(candidate_count, minimum_gap);
        add_candidate(&candidate_count,
                      peak_sample,
                      3U,
                      1000000.0f,
                      fmaxf(0.0f, peak_value - noise_mean),
                      OTDR_EVENT_UNUSED);
        candidate_count = merge_candidates(candidate_count, minimum_gap);
    }

    if (candidate_count > event_capacity) {
        candidate_count = event_capacity;
    }

    xil_printf(
        "[EVENT] noise_x100=%d std_x100=%d threshold_x100=%d "
        "noise_boundary=%d analysis_end=%d block=%d\r\n",
        (int)(noise_mean * 100.0f),
        (int)(noise_std * 100.0f),
        (int)(noise_threshold * 100.0f),
        noise_boundary * block_size,
        analysis_end * block_size,
        block_size);

    const float km_per_sample =
        OTDR_LIGHT_SPEED_M_S /
        (2.0f * config->refractive_index * sample_rate * 1000.0f);
    float total_loss = 0.0f;
    uint32_t segment_begin = analysis_start;
    for (uint32_t i = 0U; i < candidate_count; ++i) {
        const event_candidate_t *candidate = &event_candidates[i];
        otdr_event_point_t *event = &events[i];
        memset(event, 0, sizeof(*event));
        event->EventXlabel = candidate->sample;
        event->EventType = candidate->type;
        event->EventReflectLoss =
            (candidate->type == 1U || candidate->type == 3U)
                ? candidate->reflect_db
                : OTDR_EVENT_UNUSED;
        event->EventInsertLoss =
            (candidate->type == 2U)
                ? candidate->loss_db
                : OTDR_EVENT_UNUSED;
        if (candidate->type == 2U) {
            total_loss += candidate->loss_db;
        }
        event->EventTotalLoss = total_loss;

        const uint32_t event_bin = candidate->sample / block_size;
        const uint32_t segment_end =
            (event_bin > event_window) ? event_bin - event_window : event_bin;
        const float slope =
            linear_slope(event_smooth, segment_begin, segment_end);
        const float attenuation =
            -slope / (km_per_sample * (float)block_size) / 2.0f;
        event->AttenCoef =
            (attenuation >= 0.0f && attenuation <= 10.0f)
                ? attenuation
                : OTDR_EVENT_UNUSED;
        segment_begin = event_bin + event_window;
        if (segment_begin >= bin_count) {
            segment_begin = bin_count - 1U;
        }
    }

    xil_printf("[EVENT] detected=%d, analyzed_points=%d\r\n",
               candidate_count, point_count);
    for (uint32_t i = 0U; i < candidate_count; ++i) {
        xil_printf(
            "[EVENT] #%d type=%d sample=%d reflect_x100=%d "
            "loss_x100=%d atten_x100=%d total_x100=%d\r\n",
            i + 1U,
            events[i].EventType,
            events[i].EventXlabel,
            (int)(events[i].EventReflectLoss * 100.0f),
            (int)(events[i].EventInsertLoss * 100.0f),
            (int)(events[i].AttenCoef * 100.0f),
            (int)(events[i].EventTotalLoss * 100.0f));
    }

    return candidate_count;
}
