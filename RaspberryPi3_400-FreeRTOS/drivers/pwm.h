#ifndef PWM_H
#define PWM_H
/* Hardware-PWM hobby-servo control on GPIO12 (PWM0), 50 Hz, mark-space mode. */
void servo_init(void);          /* GPIO12 = PWM0, centred at 1.5 ms */
void servo_set_us(int us);      /* raw pulse width, clamped 500..2500 us */
void servo_set_angle(int deg);  /* 0..180 deg -> 1000..2000 us */
int  servo_get_angle(void);
#endif
