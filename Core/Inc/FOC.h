#ifndef FOC_H
#define FOC_H

#include "main.h"

typedef enum {
    FOC_INIT,
    FOC_CURRENT_SENSOR_CALIBRATION,
    FOC_GENERAL_TEST,
    FOC_ERROR,
    FOC_CALIBRATION,
    FOC_ALIGNMENT,
    FOC_ALIGNMENT_TEST,
    FOC_RUN,
    FOC_CHECK_CURRENT_SENSOR,
    FOC_ENCODER_TEST
} FOC_State;

/*Motor parameters for the Maxon motor*/
#define MOTOR_POLE_PAIRS 11
#define MOTOR_STATOR_RESISTANCE 0.535f // ohms
#define MOTOR_STATOR_INDUCTANCE 0.4025e-3f // henries
#define MOTOR_MAGNET_FLUX_LINKAGE 5.47e-3f // webers

#define OUTER_LOOP_FREQUENCY 2000

#define INPUT_VOLTAGE 24.0f
#define VOLTAGE_LIMIT 6.0f
#define PID_LIMIT (VOLTAGE_LIMIT * M_1_SQRT3F)
#define PID_INT_LIMIT (PID_LIMIT * 0.8f)



// Function prototypes
void FOC_Setup();
void FOC_Loop();

#endif // FOC_H