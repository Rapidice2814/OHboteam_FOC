#include <math.h>
#include <stdio.h>
#include <string.h>

#include "FOC.h"
#include "FOC_Driver.h"
#include "DRV8323_Driver.h"
#include "FOC_Utils.h"
#include "PID.h"
#include "FOC_Flash.h"
#include "WS2812b_Driver.h"
#include "Debug.h"

extern ADC_HandleTypeDef hadc1;
extern ADC_HandleTypeDef hadc2;

extern FDCAN_HandleTypeDef hfdcan1;

extern  I2C_HandleTypeDef hi2c1;

extern  SPI_HandleTypeDef hspi2;

extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim2;
extern TIM_HandleTypeDef htim3;
extern  TIM_HandleTypeDef htim4;
extern TIM_HandleTypeDef htim7;

extern UART_HandleTypeDef huart3;

extern PCD_HandleTypeDef hpcd_USB_FS;


FOC_HandleTypeDef hfoc = {0};
FOC_State Current_FOC_State = FOC_INIT;



static uint8_t Current_Sensor_Calibration_Loop();
static uint8_t Alignment_Loop();
static uint8_t General_Test_Loop();
static uint8_t Error_Loop();
static uint8_t Alignment_Test_Loop();
static uint8_t Check_Current_Sensor_Loop();
static uint8_t Encoder_Test_Loop();

static uint8_t Current_Loop();

/* flags, used for the interrupts*/
volatile uint8_t adc1_complete_flag = 0;
volatile uint8_t adc1_half_complete_flag = 0;

volatile uint8_t foc_adc1_measurement_flag = 0;
volatile uint8_t foc_outer_loop_flag = 0;
volatile uint8_t debug_loop_flag = 0;


/* ADC */
#define ADC_LOOP_ALPHA (2.0f/(CURRENT_LOOP_CLOCK_DIVIDER+1))
#define CURRENT_SENSE_CONVERSION_FACTOR 0.00100708007f // 4096.0f * 3.3f / (0.02f * 40.0f)
#define VOLTAGE_SENSE_CONVERSION_FACTOR 11.0f/1.0f * 3.3f / 4096.0f // 100:10 voltage divider

static volatile uint16_t adc1_buffer[ADC1_CHANNELS * CURRENT_LOOP_CLOCK_DIVIDER * 2] = {0};



void FOC_Setup(){
    HAL_Delay(10);
    // HAL_GetUIDw0();
    // HAL_GetUIDw1();
    // HAL_GetUIDw2();

    // FOC_FLASH_ReadData(&flash_data);
    // if(flash_data.contains_data != 1){
    //     flash_data.contains_data = 1;
    //     FOC_FLASH_WriteData(&flash_data);
    // }

    WS2812b_Setup(&htim4, TIM_CHANNEL_1);

    hfoc.motor_pole_pairs = MOTOR_POLE_PAIRS;
    hfoc.motor_stator_resistance = MOTOR_STATOR_RESISTANCE;
    hfoc.motor_stator_inductance = MOTOR_STATOR_INDUCTANCE;
    hfoc.motor_magnet_flux_linkage = MOTOR_MAGNET_FLUX_LINKAGE;


    if(DRV8323_SetPins(&hfoc.hdrv8323, &hspi2, DRV_NCS_GPIO_Port, DRV_NCS_Pin, DRV_ENABLE_GPIO_Port, DRV_ENABLE_Pin, DRV_NFAULT_GPIO_Port, DRV_NFAULT_Pin) != DRV8323_OK){
        while(1){};
    }

    if(DRV8323_Init(&hfoc.hdrv8323) != DRV8323_OK){
        while(1){
            HAL_GPIO_TogglePin(DEBUG_LED0_GPIO_Port, DEBUG_LED0_Pin);
            HAL_Delay(100);
        };
    }

    if(AS5047P_SetPins(&hfoc.has5047p, &hspi2, AS_NCS_GPIO_Port, AS_NCS_Pin) != AS5047P_OK){
        while(1){};
    }

    if(AS5047P_Init(&hfoc.has5047p) != AS5047P_OK){
        while(1){
            HAL_GPIO_TogglePin(DEBUG_LED1_GPIO_Port, DEBUG_LED1_Pin);
            HAL_Delay(100);
        };
    }

    FOC_SetEncoderPointer(&hfoc, &htim3.Instance->CNT);
    HAL_TIM_Base_Start(&htim3); //start encoder timer


    FOC_SetPWMCCRPointers(&hfoc, &htim1.Instance->CCR3, &htim1.Instance->CCR2, &htim1.Instance->CCR1, PWM_CLOCK_DIVIDER); //set the pwm ccr register pointers
    FOC_SetInputVoltage(&hfoc, INPUT_VOLTAGE); //set the input voltage, as default
    FOC_SetVoltageLimit(&hfoc, VOLTAGE_LIMIT); //set the voltage limit, as default

    FOC_SetPhaseVoltages(&hfoc, FOC_InvClarke_transform((AlphaBetaVoltages){0.0f, 0.0f}));

    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_3);
    HAL_TIM_OC_Start(&htim1, TIM_CHANNEL_4); //this triggers the adc

    HAL_GPIO_WritePin(INL_ALL_GPIO_Port, INL_ALL_Pin, GPIO_PIN_SET);

    HAL_TIM_Base_Start(&htim1); //pwm timer and also adc trigger
    HAL_ADC_Start_DMA(&hadc1, (uint32_t*)adc1_buffer, ADC1_CHANNELS * CURRENT_LOOP_CLOCK_DIVIDER * 2); //start adc in dma mode

    
    HAL_TIM_Base_Start(&htim2); //timer for the counter
    HAL_TIM_Base_Start_IT(&htim7); //timer for the debug


    /* PID controllers */
    PID_Init(&hfoc.pid_current_d, (1.0f/CURRENT_LOOP_FREQUENCY), 0.01f, -PID_LIMIT, PID_LIMIT, -PID_INT_LIMIT, PID_INT_LIMIT, &hfoc.flash_data.PID_gains_d);
    PID_Init(&hfoc.pid_current_q, (1.0f/CURRENT_LOOP_FREQUENCY), 0.01f, -PID_LIMIT, PID_LIMIT, -PID_INT_LIMIT, PID_INT_LIMIT, &hfoc.flash_data.PID_gains_q);

    // PID_Init(&hfoc.pid_speed, (1.0f/DEBUG_FREQUENCY), 0.01f, -2.0f, 2.0f, -2.0f, 2.0f);
    // PID_SetK(&hfoc.pid_speed, 0.01f, 0.0f, 0.0f);

    // hfoc.speed_setpoint = 300.0f;

    hfoc.flash_data.PID_gains_d.Kp = 0.0f;
    hfoc.flash_data.PID_gains_d.Ki = 0.0f;
    hfoc.flash_data.PID_gains_d.Kd = 0.0f;

    hfoc.flash_data.PID_gains_q.Kp = 0.2f;
    hfoc.flash_data.PID_gains_q.Ki = 1.0f;
    hfoc.flash_data.PID_gains_q.Kd = 0.0f;


    /* UART */
    Debug_Setup();

}


static uint32_t start_time = 0;
static uint32_t execution_time = 0;
static uint32_t max_execution_time = 0;

static uint32_t adc1_start_time = 0;
static uint32_t adc1_time = 0;
static uint32_t max_adc1_time = 0;


char usart3_tx_buffer[200];
uint8_t timeout_flag = 0;

void FOC_Loop(){
    start_time = __HAL_TIM_GET_COUNTER(&htim2);

    if(adc1_half_complete_flag || adc1_complete_flag){ //This loop runs at (PWM frequency / CURRENT_LOOP_CLOCK_SCALER)
        adc1_start_time = __HAL_TIM_GET_COUNTER(&htim2);

            int start_index = adc1_half_complete_flag ? 0 : CURRENT_LOOP_CLOCK_DIVIDER * ADC1_CHANNELS;
            int end_index = start_index + CURRENT_LOOP_CLOCK_DIVIDER * ADC1_CHANNELS;

            for (int i = start_index; i < end_index; i += ADC1_CHANNELS) {
                hfoc.phase_current.a = hfoc.phase_current.a * (1 - ADC_LOOP_ALPHA) + ADC_LOOP_ALPHA * CURRENT_SENSE_CONVERSION_FACTOR * (float)(adc1_buffer[i + 0] - 2048) - hfoc.phase_current_offset.a;
                hfoc.phase_current.b = hfoc.phase_current.b * (1 - ADC_LOOP_ALPHA) + ADC_LOOP_ALPHA * CURRENT_SENSE_CONVERSION_FACTOR * (float)(adc1_buffer[i + 1] - 2048) - hfoc.phase_current_offset.b;
                hfoc.phase_current.c = hfoc.phase_current.c * (1 - ADC_LOOP_ALPHA) + ADC_LOOP_ALPHA * CURRENT_SENSE_CONVERSION_FACTOR * (float)(adc1_buffer[i + 2] - 2048) - hfoc.phase_current_offset.c;
                hfoc.vbus            = hfoc.vbus            * (1 - ADC_LOOP_ALPHA) + ADC_LOOP_ALPHA * VOLTAGE_SENSE_CONVERSION_FACTOR * (float)(adc1_buffer[i + 3] ) - hfoc.vbus_offset;
            }

            HAL_GPIO_TogglePin(PB2_GPIO_Port, PB2_Pin);

            foc_adc1_measurement_flag = 1; //triggered after the adc conversion is complete (either first or second half of the buffer)
            adc1_half_complete_flag = 0;
            adc1_complete_flag = 0;

        adc1_time = __HAL_TIM_GET_COUNTER(&htim2) - adc1_start_time;
        if (adc1_time > max_adc1_time) {
            max_adc1_time = adc1_time;
        }
    }

    if(foc_adc1_measurement_flag){
        switch(Current_FOC_State){
        
            case FOC_INIT:
                __NOP();
                Current_FOC_State = FOC_CURRENT_SENSOR_CALIBRATION;
                break;
            case FOC_CURRENT_SENSOR_CALIBRATION:
                if(Current_Sensor_Calibration_Loop()){
                    Current_FOC_State = FOC_ALIGNMENT;
                    hfoc.adc_calibrated = 1;
                }
                break;
            case FOC_GENERAL_TEST:
                if(General_Test_Loop()){
                    Current_FOC_State = FOC_RUN;
                    __NOP();
                }
                break;
            case FOC_CALIBRATION:
                break;
            case FOC_ALIGNMENT:
                if(Alignment_Loop()){
                    Current_FOC_State = FOC_RUN;
                    // Current_FOC_State = FOC_ENCODER_TEST;
                }
                break;
            case FOC_ALIGNMENT_TEST:
                if(Alignment_Test_Loop()){
                    Current_FOC_State = FOC_GENERAL_TEST;
                }
                break;
            case FOC_CHECK_CURRENT_SENSOR:
                if(Check_Current_Sensor_Loop()){
                    Current_FOC_State = FOC_GENERAL_TEST;
                }
                break;
            case FOC_ENCODER_TEST:
                if(Encoder_Test_Loop()){
                    // Current_FOC_State = FOC_GENERAL_TEST;
                }
                break;
            case FOC_RUN:
                Current_Loop();
                Debug_Loop();

                if(debug_loop_flag){
                    Debug_Queue(&hfoc);
                    debug_loop_flag = 0;
                }
                break;
            case FOC_ERROR:
                if(Error_Loop()){
                    // Current_FOC_State = FOC_GENERAL_TEST;
                }
                break;
            
        }
    }
    

    execution_time = __HAL_TIM_GET_COUNTER(&htim2) - start_time;
    if (execution_time > max_execution_time) {
        max_execution_time = execution_time;
    }
    if(execution_time > 110){
        // snprintf(usart3_tx_buffer, sizeof(usart3_tx_buffer), "Execution time: %lu us\r\n", execution_time);
        // HAL_UART_Transmit_DMA(&huart3, (uint8_t*)usart3_tx_buffer, strlen(usart3_tx_buffer));
        __NOP();
    }
}


static uint8_t Current_Loop(){
    if(foc_adc1_measurement_flag){
        if(DRV8323_CheckFault(&hfoc.hdrv8323)){ //check for motor driver fault
            DRV8323_Disable(&hfoc.hdrv8323); //disable the driver
            HAL_GPIO_WritePin(INL_ALL_GPIO_Port, INL_ALL_Pin, 0); //disable the inverter
            FOC_SetPhaseVoltages(&hfoc, FOC_InvClarke_transform((AlphaBetaVoltages){0.0f, 0.0f}));
            Current_FOC_State = FOC_ERROR;
        }
    
        FOC_UpdateEncoderAngle(&hfoc);
        FOC_UpdateEncoderSpeed(&hfoc, CURRENT_LOOP_FREQUENCY, 0.01f);
    
        hfoc.ab_current = FOC_Clarke_transform(hfoc.phase_current);
        hfoc.dq_current = FOC_Park_transform(hfoc.ab_current, hfoc.encoder_angle_electrical);
    
        hfoc.dq_voltage.d = PID_Update(&hfoc.pid_current_d, hfoc.dq_current_setpoint.d, hfoc.dq_current.d);
        hfoc.dq_voltage.q = PID_Update(&hfoc.pid_current_q, hfoc.dq_current_setpoint.q, hfoc.dq_current.q);
    
        hfoc.ab_voltage = FOC_InvPark_transform(hfoc.dq_voltage, hfoc.encoder_angle_electrical);
        PhaseVoltages phase_voltages = FOC_InvClarke_transform(hfoc.ab_voltage);
        FOC_SetPhaseVoltages(&hfoc, phase_voltages);
        // FOC_SetPhaseVoltages(&hfoc, FOC_InvClarke_transform((AlphaBetaVoltages){0.4f, 0.0f}));

        foc_adc1_measurement_flag = 0;
    }
    return 0;
}


/**
  * @brief Current sensor calibration loop
  * @note This function is used to calibrate the current sensor. It takes 100 samples of the current sensor and calculates the offset.
  * @param None
  * @retval uint8_t: 0 if the calibration is not complete, 1 if the calibration is complete
  */
static uint8_t Current_Sensor_Calibration_Loop(){

    static uint8_t step = 0;
    static uint32_t next_step_time = 0;
    static uint8_t measurement_step_counter = 0;

    switch(step){
        case 0:
            if(HAL_GetTick() >= next_step_time){

                DRV8323_CSACALStart(&hfoc.hdrv8323);

                step++;
                next_step_time = HAL_GetTick() + 1; //wait 1ms before the next step
            }
            break;
        case 1:
            if(HAL_GetTick() >= next_step_time){
                if(foc_adc1_measurement_flag){
                    measurement_step_counter++;
    
                    hfoc.phase_current_offset.a += hfoc.phase_current.a;
                    hfoc.phase_current_offset.b += hfoc.phase_current.b;
                    hfoc.phase_current_offset.c += hfoc.phase_current.c;
    
                    foc_adc1_measurement_flag = 0;
                    if(measurement_step_counter >= 100){

                        DRV8323_CSACALStop(&hfoc.hdrv8323);
                        measurement_step_counter = 0; //reset the counter

                        step++;
                        next_step_time = HAL_GetTick() + 1; //wait 1ms before the next step
                    }
                }
            }
            break;
        default:
            if(HAL_GetTick() >= next_step_time){
                step = 0;
                return 1; // complete
            }
            break;
    }

    return 0;
}

/**
  * @brief 
  * @note 
  * @param None
  * @retval uint8_t: 0 if the calibration is not complete, 1 if the calibration is complete
  */
 static uint8_t Alignment_Loop(){

    static uint8_t step = 0;
    static uint32_t next_step_time = 0;
    
    switch(step){
        case 0:
            if(HAL_GetTick() >= next_step_time){
                FOC_SetPhaseVoltages(&hfoc, FOC_InvClarke_transform((AlphaBetaVoltages){0.5f, 0.0f}));
                step++;
                next_step_time = HAL_GetTick() + 500; //wait before the next step
            }
            break;
        case 1:
            if(HAL_GetTick() >= next_step_time){
                FOC_SetEncoderZero(&hfoc);
                FOC_UpdateEncoderAngle(&hfoc);
                step++;
                next_step_time = HAL_GetTick() + 100; //wait before the next step
            }
            break;
        case 2:
            if(HAL_GetTick() >= next_step_time){
                FOC_SetPhaseVoltages(&hfoc, FOC_InvClarke_transform((AlphaBetaVoltages){0.0f, 0.0f}));
                step++;
                next_step_time = HAL_GetTick() + 100; //wait before the next step
            }
            break;
        default:
            if(HAL_GetTick() >= next_step_time){
                step = 0;
                return 1; // complete
            }
            break;
        }

    return 0;
    }

/**
  * @brief 
  * @note 
  * @param None
  * @retval uint8_t: 0 if the calibration is not complete, 1 if the calibration is complete
  */
static uint8_t General_Test_Loop(){

    static uint8_t step = 0;
    static uint32_t next_step_time = 0;

    switch(step){
        case 0:
            if(HAL_GetTick() >= next_step_time){
                FOC_UpdateEncoderAngle(&hfoc);
                
                HAL_GPIO_WritePin(DEBUG_LED0_GPIO_Port, DEBUG_LED0_Pin, 1);
                for(int i = 0; i < WS2812B_NUMBER_OF_LEDS; i++){
                    WS2812b_SetColor(i, 0, 25, 0); //green
                }
                WS2812b_Send();

                step++;
                next_step_time = HAL_GetTick() + 100; //wait 1ms before the next step
            }
            break;
        case 1:
            if(HAL_GetTick() >= next_step_time){

                HAL_GPIO_WritePin(DEBUG_LED0_GPIO_Port, DEBUG_LED0_Pin, 0);
                for(int i = 0; i < WS2812B_NUMBER_OF_LEDS; i++){
                    WS2812b_SetColor(i, 0, 0, 0); 
                }
                WS2812b_Send();
                
                step++;
                next_step_time = HAL_GetTick() + 100; //wait 1ms before the next step
            }
            break;
        default:
            if(HAL_GetTick() >= next_step_time){
                step = 0;
                return 1; // complete
            }
            break;
    }

return 0;
}

/**
  * @brief 
  * @note 
  * @param None
  * @retval uint8_t: 0 if the calibration is not complete, 1 if the calibration is complete
  */
static uint8_t Error_Loop(){

    static uint8_t step = 0;
    static uint32_t next_step_time = 0;

    switch(step){
        case 0:
            if(HAL_GetTick() >= next_step_time){

                FOC_UpdateEncoderAngle(&hfoc);
                
                HAL_GPIO_WritePin(DEBUG_LED0_GPIO_Port, DEBUG_LED0_Pin, 1);
                for(int i = 0; i < WS2812B_NUMBER_OF_LEDS; i++){
                    WS2812b_SetColor(i, 25, 0, 0); //green
                }
                WS2812b_Send();

                step++;
                next_step_time = HAL_GetTick() + 100; //wait 1ms before the next step
            }
            break;
        case 1:
            if(HAL_GetTick() >= next_step_time){

                HAL_GPIO_WritePin(DEBUG_LED0_GPIO_Port, DEBUG_LED0_Pin, 0);
                for(int i = 0; i < WS2812B_NUMBER_OF_LEDS; i++){
                    WS2812b_SetColor(i, 0, 0, 0); 
                }
                WS2812b_Send();
                
                step++;
                next_step_time = HAL_GetTick() + 100; //wait 1ms before the next step
            }
            break;
        default:
            if(HAL_GetTick() >= next_step_time){
                step = 0;
                return 1; // complete
            }
            break;
    }

return 0;
}


/**
  * @brief 
  * @note 
  * @param None
  * @retval uint8_t: 0 if the calibration is not complete, 1 if the calibration is complete
  */
static uint8_t Alignment_Test_Loop(){

    static uint8_t step = 0;
    static uint32_t next_step_time = 0;
    static uint32_t substep_counter = 0;

    static float reference_angle = 0.0f;
    static float reference_electrical_angle = 0.0f;
    static float magnitude = 0.6f;
    static uint8_t direction = 0;
    static uint8_t dir_swapped = 0;

    static float current_angle = 0.0f;
    static float current_magnitude = 0.0f;

    static float abs_diff_cw = 0.0f;
    static float abs_diff_ccw = 0.0f;

    switch(step){
        case 0:
            if(HAL_GetTick() >= next_step_time){
                FOC_SetPhaseVoltages(&hfoc, FOC_InvClarke_transform((AlphaBetaVoltages){0.3f, 0.0f}));
                step++;
                next_step_time = HAL_GetTick() + 100; 
            }
            break;
        case 1:
            if(HAL_GetTick() >= next_step_time){
                // FOC_SetEncoderZero(&hfoc);
                FOC_UpdateEncoderAngle(&hfoc);
                step++;
                next_step_time = HAL_GetTick() + 10;
            }
            break;

        case 2:
            if(HAL_GetTick() >= next_step_time){

                AlphaBetaCurrents Iab = FOC_Clarke_transform(hfoc.phase_current);
                current_angle = atan2f(Iab.beta, Iab.alpha);
                normalize_angle(&current_angle);
                current_magnitude = sqrtf(Iab.alpha * Iab.alpha + Iab.beta * Iab.beta);

                FOC_UpdateEncoderAngle(&hfoc);
                FOC_UpdateEncoderSpeed(&hfoc, 1.0f, 2.0f/(50.0f + 1.0f));

                uint16_t ap5047p_angle = 0.0f;
                AS5047P_GetAngle(&hfoc.has5047p, &ap5047p_angle);
                float as5047p_angle = ((float)ap5047p_angle / 16384.0f) * 2.0f * M_PIF; //convert to radians
                
                as5047p_angle = as5047p_angle - hfoc.flash_data.encoder_angle_mechanical_offset;
                normalize_angle(&as5047p_angle);
                float as5047p_angle_electrical = as5047p_angle * hfoc.motor_pole_pairs;
                normalize_angle(&as5047p_angle_electrical);

                float diff1 = reference_electrical_angle - hfoc.encoder_angle_electrical;
                normalize_angle2(&diff1);
                float diff2 = reference_electrical_angle - as5047p_angle_electrical;
                normalize_angle2(&diff2);

                

                snprintf(usart3_tx_buffer, sizeof(usart3_tx_buffer), "RefM:%d, AngM:%d, Ang2M:%d, RefE:%d, AngE:%d, Ang2E:%d, CAng:%d, D1:%d, D2:%d\n", 
                (int)(reference_angle * 1000), (int)(hfoc.encoder_angle_mechanical * 1000), 
                (int)(as5047p_angle * 1000), (int)(reference_electrical_angle * 1000), 
                (int)(hfoc.encoder_angle_electrical * 1000), (int)(as5047p_angle_electrical * 1000),
                (int)(current_angle * 1000), (int)(diff1 * 1000), (int)(diff2 * 1000) 
                );

                // snprintf(usart3_tx_buffer, sizeof(usart3_tx_buffer), "RefM:%d, AngM:%d\n", 
                // (int)(reference_angle * 1000), (int)(hfoc.encoder_angle_mechanical * 1000)
                // );

                HAL_UART_Transmit_DMA(&huart3, (uint8_t*)usart3_tx_buffer, strlen(usart3_tx_buffer));


                if(!direction){//cw
                    reference_angle += 2*M_PIF / 200.0f;
                    abs_diff_cw += fabsf(diff1) / 200.0f;
                }else{//ccw
                    reference_angle -= 2*M_PIF / 200.0f;
                    abs_diff_ccw += fabsf(diff1) / 200.0f;
                }
                normalize_angle(&reference_angle);
                reference_electrical_angle = reference_angle * hfoc.motor_pole_pairs;
                normalize_angle(&reference_electrical_angle);


                AlphaBetaVoltages Vab;
                Vab.alpha = magnitude * cosf(reference_electrical_angle);
                Vab.beta = magnitude * sinf(reference_electrical_angle);
                PhaseVoltages phase_voltages = FOC_InvClarke_transform(Vab);
                FOC_SetPhaseVoltages(&hfoc, phase_voltages);


                substep_counter++;
                if(substep_counter > 200){
                    substep_counter = 0;
                    HAL_GPIO_WritePin(DEBUG_LED0_GPIO_Port, DEBUG_LED0_Pin, 1);

                    if(!direction){
                        if(hfoc.encoder_speed_electrical < 0.0f){
                            dir_swapped = 1;
                        }
                        direction = 1;
                    }else{
                        if(hfoc.encoder_speed_electrical > 0.0f){
                            if(dir_swapped){
                               hfoc.flash_data.motor_direction_swapped = !hfoc.flash_data.motor_direction_swapped;
                            }else{
                                __NOP(); //error
                            }
                        }
                        direction = 0;
                        step++;
                    }
                    next_step_time = HAL_GetTick() + 100;
                }else{
                    next_step_time = HAL_GetTick() + 10; 
                }
            }
            break;
        case 3:
            if(HAL_GetTick() >= next_step_time){

                FOC_SetPhaseVoltages(&hfoc, FOC_InvClarke_transform((AlphaBetaVoltages){0.0f, 0.0f}));

                snprintf(usart3_tx_buffer, sizeof(usart3_tx_buffer), "Abs Diff CW:%d, CCW:%d\n", (int)(abs_diff_cw * 1000), (int)(abs_diff_ccw * 1000));
                HAL_UART_Transmit_DMA(&huart3, (uint8_t*)usart3_tx_buffer, strlen(usart3_tx_buffer));
                abs_diff_cw = 0.0f;
                abs_diff_ccw = 0.0f;

                step++;
                next_step_time = HAL_GetTick() + 5000;
            }
            break;

        default:
            if(HAL_GetTick() >= next_step_time){
                step = 0;
                return 1; // complete
            }
            break;
    }

return 0;
}

/**
  * @brief 
  * @note 
  * @param None
  * @retval uint8_t: 0 if the calibration is not complete, 1 if the calibration is complete
  */
static uint8_t Check_Current_Sensor_Loop(){

static uint8_t step = 0;
static uint32_t next_step_time = 0;


    switch(step){

        case 0:
            if(HAL_GetTick() >= next_step_time){

                FOC_SetPhaseVoltages(&hfoc, (PhaseVoltages){0.1f, 0.0f, 0.0f});

                step++;
                next_step_time = HAL_GetTick() + 10;
            }
            break;
        case 1:
            if(HAL_GetTick() >= next_step_time){

                FOC_SetPhaseVoltages(&hfoc, (PhaseVoltages){0.0f, 0.1f, 0.0f});

                step++;
                next_step_time = HAL_GetTick() + 10;
            }
            break;
        case 2:
            if(HAL_GetTick() >= next_step_time){

                FOC_SetPhaseVoltages(&hfoc, (PhaseVoltages){0.0f, 0.0f, 0.1f});

                step++;
                next_step_time = HAL_GetTick() + 10;
            }
            break;

        default:
            if(HAL_GetTick() >= next_step_time){
                step = 0;
                return 1; // complete
            }
            break;
    }

return 0;
}

/**
  * @brief 
  * @note 
  * @param None
  * @retval uint8_t: 0 if the calibration is not complete, 1 if the calibration is complete
  */
static uint8_t Encoder_Test_Loop(){

    static uint8_t step = 0;
    static uint32_t next_step_time = 0;

    switch(step){
        static uint16_t diaagc = 0;
        static uint16_t cmag = 0;
        static float angle = 0.0f;
        static float angle_raw = 0.0f;
        case 0:
            if(HAL_GetTick() >= next_step_time){
                AS5047P_GetDIAAGC(&hfoc.has5047p, &diaagc);
                AS5047P_GetCMAG(&hfoc.has5047p, &cmag);

                uint16_t ap5047p_angle = 0.0f;
                AS5047P_GetAngle(&hfoc.has5047p, &ap5047p_angle);
                angle = ((float)ap5047p_angle / 16384.0f) * 2.0f * M_PIF; //convert to radians

                uint16_t ap5047p_angle_raw = 0.0f;
                AS5047P_GetAngle_Raw(&hfoc.has5047p, &ap5047p_angle_raw);
                angle_raw = ((float)ap5047p_angle / 16384.0f) * 2.0f * M_PIF; //convert to radians

                FOC_UpdateEncoderAngle(&hfoc);

                snprintf(usart3_tx_buffer, sizeof(usart3_tx_buffer), "Normal:%d, Raw:%d, Comp:%d\n", (int)(hfoc.encoder_angle_mechanical * 1000), (int)(angle_raw * 1000), (int)(angle * 1000));
                HAL_UART_Transmit_DMA(&huart3, (uint8_t*)usart3_tx_buffer, strlen(usart3_tx_buffer));
                // step++;
                next_step_time = HAL_GetTick() + 100; //wait 1ms before the next step
            }
            break;
        case 1:
            if(HAL_GetTick() >= next_step_time){

                
                step++;
                next_step_time = HAL_GetTick() + 100; //wait 1ms before the next step
            }
            break;
        default:
            if(HAL_GetTick() >= next_step_time){
                step = 0;
                return 1; // complete
            }
            break;
    }

return 0;
}





// /**
//   * @brief 
//   * @note 
//   * @param None
//   * @retval uint8_t: 0 if the calibration is not complete, 1 if the calibration is complete
//   */
//  static uint8_t General_Test_Loop(){

//     static uint8_t step = 0;
//     static uint32_t next_step_time = 0;

//     switch(step){
//         case 0:
//             if(HAL_GetTick() >= next_step_time){
                
//                 step++;
//                 next_step_time = HAL_GetTick() + 100; //wait 1ms before the next step
//             }
//             break;
//         case 1:
//             if(HAL_GetTick() >= next_step_time){

                
//                 step++;
//                 next_step_time = HAL_GetTick() + 100; //wait 1ms before the next step
//             }
//             break;
//         default:
//             if(HAL_GetTick() >= next_step_time){
//                 step = 0;
//                 return 1; // complete
//             }
//             break;
//     }

// return 0;
// }

























void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) { //when the adc conversion is complete
    if(hadc->Instance == ADC1){ 
        if(!adc1_complete_flag){
            adc1_complete_flag = 1;  
        }
    }
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc) { //when the adc conversion is half complete
    if(hadc->Instance == ADC1){
        if(!adc1_half_complete_flag){
            adc1_half_complete_flag = 1;
        }
    }
    
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim){
    if(htim->Instance == TIM6){
        // foc_adc1_measurement_flag = 1;
    } else if(htim->Instance == TIM7){
        debug_loop_flag = 1;
    } else if(htim->Instance == TIM17){
        // debug_loop_flag = 1;
    }
}

void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim){
    if(htim == &htim4){
        WS2812b_PulseFinishedCallback();
    }
}