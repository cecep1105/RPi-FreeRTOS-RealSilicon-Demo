#ifndef STEPPER_H
#define STEPPER_H
/* 28BYJ-48 unipolar stepper via a ULN2003 driver, half-step drive.
 * IN1..IN4 -> GPIO 5, 6, 16, 26 (project's free pins; PWM1/GPIO13 left free). */
void stepper_init(void);
void stepper_phase(int phase);   /* energize half-step phase 0..7 */
void stepper_release(void);      /* de-energize all coils (no holding torque) */
#endif
