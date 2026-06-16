#ifndef RCOS_TABLES_H
#define RCOS_TABLES_H

#include <stdint.h>

typedef struct {
    int sps;
    int len;
    const float *coef;
} rcos_table_t;

extern const float rcos_sps1[7];
extern const float rcos_sps2[13];
extern const float rcos_sps3[19];
extern const float rcos_sps4[25];
extern const float rcos_sps5[31];
extern const float rcos_sps6[37];
extern const float rcos_sps7[43];
extern const float rcos_sps8[49];
extern const float rcos_sps9[55];
extern const float rcos_sps10[61];
extern const float rcos_sps20[121];

extern const rcos_table_t rcos_tables[11];
#define RCOS_TABLE_COUNT 11

const rcos_table_t *find_rcos_table(int sps);

#endif
