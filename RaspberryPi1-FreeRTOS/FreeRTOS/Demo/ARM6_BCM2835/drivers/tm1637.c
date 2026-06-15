#include "tm1637.h"
#include "gpio.h"
#include "timer.h"

/* Open-drain emulation: drive low by switching the pin to OUTPUT (latch
   pre-cleared in init); release high by switching to INPUT (external/soft
   pull-up). */
static void tm_delay(void)    { delay(TM_DELAY); }
static void tm_clk_high(void) { gpio_set_function(TM_CLK, GPIO_FUNC_INPUT);  }
static void tm_clk_low(void)  { gpio_set_function(TM_CLK, GPIO_FUNC_OUTPUT); }
static void tm_dio_high(void) { gpio_set_function(TM_DIO, GPIO_FUNC_INPUT);  }
static void tm_dio_low(void)  { gpio_set_function(TM_DIO, GPIO_FUNC_OUTPUT); }
static int  tm_dio_read(void) { return gpio_read(TM_DIO); }

static void tm_start(void){ tm_clk_high(); tm_dio_high(); tm_delay(); tm_dio_low(); tm_delay(); }
static void tm_stop(void){ tm_clk_low(); tm_delay(); tm_dio_low(); tm_delay(); tm_clk_high(); tm_delay(); tm_dio_high(); tm_delay(); }

static int tm_write(uint8_t b){
    for (int i=0;i<8;i++){ tm_clk_low(); tm_delay(); if(b&1) tm_dio_high(); else tm_dio_low(); tm_delay(); tm_clk_high(); tm_delay(); b>>=1; }
    tm_clk_low(); tm_dio_high(); tm_delay(); tm_clk_high(); tm_delay();
    int ack=(tm_dio_read()==0); tm_clk_low(); tm_delay(); return ack;
}

void tm1637_init(void){
    gpio_clear((1u<<TM_CLK)|(1u<<TM_DIO));
    gpio_set_pull(TM_CLK, GPIO_PULL_UP);
    gpio_set_pull(TM_DIO, GPIO_PULL_UP);
    tm_clk_high(); tm_dio_high();
}

static const uint8_t tmfont[10] = {0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F};

static void tm1637_show(const uint8_t seg[4]){
    tm_start(); tm_write(0x40); tm_stop();
    tm_start(); tm_write(0xC0); for(int i=0;i<4;i++) tm_write(seg[i]); tm_stop();
    tm_start(); tm_write(0x88|TM_BRIGHT); tm_stop();
}

void tm_number(uint32_t val){
    if(val>9999)val=9999;
    uint8_t d[4];
    d[3]=val%10;val/=10; d[2]=val%10;val/=10; d[1]=val%10;val/=10; d[0]=val%10;
    uint8_t seg[4]; int blank=1;
    for(int k=0;k<4;k++){ if(d[k])blank=0; seg[k]=(blank&&k<3)?0x00:tmfont[d[k]]; }
    tm1637_show(seg);
}

void tm_time(int hh,int mm,int colon){
    uint8_t seg[4];
    seg[0]=tmfont[(hh/10)%10]; seg[1]=tmfont[hh%10]; if(colon) seg[1]|=0x80;
    seg[2]=tmfont[(mm/10)%10]; seg[3]=tmfont[mm%10];
    tm1637_show(seg);
}

void tm_dashes(void){ uint8_t seg[4]={0x40,0x40,0x40,0x40}; tm1637_show(seg); }
