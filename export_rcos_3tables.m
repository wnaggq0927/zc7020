% Export MATLAB rcosdesign coefficients for Vitis C tables.
% Common pulse widths:
%   250 MHz: 8,12,16,20,24,28,32,36,40,80 ns -> sps = 2:10,20
%   125 MHz: 8,12,16,20,24,28,32,36,40,80 ns -> sps = 1,2,3,4,5,10

clear; clc;

rolloff = 1;
span = 6;
pulse_width_ns = [8 12 16 20 24 28 32 36 40 80];
sample_period_ns = [4 8]; % 250 MHz and 125 MHz downsample
sps_list = [];
for p = pulse_width_ns
    for ts = sample_period_ns
        sps_list(end + 1) = max(1, round(p / ts)); %#ok<SAGROW>
    end
end
sps_list = sort(unique(sps_list));
name_list = "sps" + string(sps_list);

out_h = 'rcos_tables_generated.h';
out_txt = 'rcos_tables_generated.txt';

fid_h = fopen(out_h, 'w');
fid_t = fopen(out_txt, 'w');

fprintf(fid_h, '#ifndef RCOS_3TABLES_FROM_MATLAB_H\n');
fprintf(fid_h, '#define RCOS_3TABLES_FROM_MATLAB_H\n\n');
fprintf(fid_h, '#include <stdint.h>\n\n');

fprintf(fid_h, 'typedef struct {\n');
fprintf(fid_h, '    int sps;\n');
fprintf(fid_h, '    int len;\n');
fprintf(fid_h, '    const float *coef;\n');
fprintf(fid_h, '} rcos_table_t;\n\n');

for k = 1:numel(sps_list)
    sps = sps_list(k);
    tag = char(name_list(k));

    h_rc = rcosdesign(rolloff, span, sps, 'normal');
    h_rc = h_rc / max(h_rc);
    h_rc = h_rc(:).';

    fprintf('Table %s: sps=%d, len=%d\n', tag, sps, numel(h_rc));

    fprintf(fid_t, '===== %s, sps=%d, len=%d =====\n', tag, sps, numel(h_rc));
    fprintf(fid_t, 'static const float rcos_%s[%d] = {\n', tag, numel(h_rc));

    fprintf(fid_h, 'static const float rcos_%s[%d] = {\n', tag, numel(h_rc));

    for i = 1:numel(h_rc)
        if i < numel(h_rc)
            suffix = ',';
        else
            suffix = '';
        end

        fprintf(fid_h, '    %.9ff%s\n', h_rc(i), suffix);
        fprintf(fid_t, '    %.9ff%s\n', h_rc(i), suffix);
    end

    fprintf(fid_h, '};\n\n');
    fprintf(fid_t, '};\n\n');
end

fprintf(fid_h, 'static const rcos_table_t rcos_tables[] = {\n');
for k = 1:numel(sps_list)
    sps = sps_list(k);
    tag = char(name_list(k));
    len = span * sps + 1;
    fprintf(fid_h, '    {%d, %d, rcos_%s},\n', sps, len, tag);
end
fprintf(fid_h, '};\n\n');

fprintf(fid_h, '#define RCOS_TABLE_COUNT ((int)(sizeof(rcos_tables) / sizeof(rcos_tables[0])))\n\n');
fprintf(fid_h, '#endif\n');

fclose(fid_h);
fclose(fid_t);

fprintf('\nGenerated:\n');
fprintf('  %s\n', out_h);
fprintf('  %s\n', out_txt);
