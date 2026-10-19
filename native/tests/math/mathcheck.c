// mathcheck.c — compare jsmath.c against Node's Math on the inputs in in.txt / js.txt, which
// difftest.sh generates. Prints one mismatch count per function and exits 1 on any mismatch.
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
double js_sin(double),js_cos(double),js_tan(double),js_asin(double),js_acos(double),js_atan(double),js_exp(double),js_log(double),js_log2(double),js_log10(double),js_sinh(double),js_cosh(double),js_tanh(double),js_cbrt(double),js_expm1(double),js_log1p(double),js_atan2(double,double),js_pow(double,double);
int main(int argc,char**argv){if(argc<3)return 2;FILE*f=fopen(argv[1],"r");FILE*g=fopen(argv[2],"r");int any=0;char a[256],b[4096];const char*n[]={"sin","cos","tan","asin","acos","atan","exp","log","log2","log10","sinh","cosh","tanh","cbrt","expm1","log1p","atan2","pow"};int bad[18]={0};int tot=0;
while(fgets(a,256,f)&&fgets(b,4096,g)){double x,y;sscanf(a,"%lf %lf",&x,&y);double ex=fabs(x)>800?fmod(x,800):x;double c[18]={js_sin(x),js_cos(x),js_tan(x),js_asin(x/200),js_acos(x/200),js_atan(x),js_exp(ex),js_log(x),js_log2(x),js_log10(x),js_sinh(ex),js_cosh(ex),js_tanh(x),js_cbrt(x),js_expm1(ex),js_log1p(x),js_atan2(x,y),js_pow(fmod(fabs(x),50),y)};char*p=b;for(int i=0;i<18;i++){char*e;while(*p==' ')p++;double j;if(!strncmp(p,"NaN",3)){j=NAN;e=p+3;}else if(!strncmp(p,"-Infinity",9)){j=-INFINITY;e=p+9;}else if(!strncmp(p,"Infinity",8)){j=INFINITY;e=p+8;}else j=strtod(p,&e);p=e; if(!(j==c[i]||(isnan(j)&&isnan(c[i])))){if(bad[i]<2)printf("%s(%.17g,%.17g): js %.17g c %.17g\n",n[i],x,y,j,c[i]);bad[i]++;}}tot++;}
for(int i=0;i<18;i++){printf("%s:%d ",n[i],bad[i]);any|=bad[i];}printf("of %d\n",tot);return any?1:0;}
