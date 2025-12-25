#ifndef  RAMCDC_H
#define  RAMCDC_H

void ramcdc_init(int);

int ramcdc(unsigned char *p, int n);
int ramcdc_avx_256(unsigned char *p, int n);
int ramcdc_avx_512(unsigned char *p, int n);
#endif