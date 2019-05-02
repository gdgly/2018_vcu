#include "drive_by_wire.h"
#include "stm32f7xx_hal.h"
#include "drive_by_wire_mock.h"
#include "stdbool.h"
#include "FreeRTOS.h"
#include "task.h"
#include "debug.h"
#include "state_machine.h"
#include "timers.h"
#include "VCU_F7_dtc.h"
#include "VCU_F7_can.h"
#include "canReceive.h"
#include "brakeAndThrottle.h"
#include "watchdog.h"

FSM_Handle_Struct fsmHandle;
TimerHandle_t throttleUpdateTimer;

uint32_t runSelftTests(uint32_t event);
uint32_t EM_Enable(uint32_t event);
uint32_t EM_Fault(uint32_t event);
uint32_t EM_Update_Throttle(uint32_t event);
uint32_t DefaultTransition(uint32_t event);

void throttleTimerCallback(TimerHandle_t timer);
HAL_StatusTypeDef MotorStart();
HAL_StatusTypeDef MotorStop();

Transition_t transitions[] = {
    { STATE_Self_Check, EV_Init, &runSelftTests },
    { STATE_EM_Disable, EV_EM_Toggle, &EM_Enable },
    { STATE_EM_Disable, EV_Bps_Fail, &EM_Fault },
    { STATE_EM_Disable, EV_Hv_Disable, &EM_Fault },
    { STATE_EM_Disable, EV_Brake_Pressure_Fault, &EM_Fault },
    { STATE_EM_Disable, EV_DCU_Can_Timeout, &EM_Fault },
    { STATE_EM_Disable, EV_Throttle_Failure, &EM_Fault },
    { STATE_EM_Disable, EV_EM_Toggle, &EM_Fault },
    { STATE_EM_Enable, EV_Bps_Fail, &EM_Fault },
    { STATE_EM_Enable, EV_Hv_Disable, &EM_Fault },
    { STATE_EM_Enable, EV_Brake_Pressure_Fault, &EM_Fault },
    { STATE_EM_Enable, EV_DCU_Can_Timeout, &EM_Fault },
    { STATE_EM_Enable, EV_Throttle_Failure, &EM_Fault },
    { STATE_EM_Enable, EV_EM_Toggle, &EM_Fault },
    { STATE_EM_Enable, EV_Throttle_Poll, &EM_Update_Throttle },
    { STATE_ANY, EV_Fatal, &EM_Fault },
    { STATE_ANY, EV_ANY, &DefaultTransition}
};

HAL_StatusTypeDef driveByWireInit()
{
    FSM_Init_Struct init;

    throttleUpdateTimer = xTimerCreate("ThrottleTimer",
                                       pdMS_TO_TICKS(THROTTLE_POLL_TIME_MS),
                                       pdTRUE /* Auto Reload */,
                                       0,
                                       throttleTimerCallback);

    if (throttleUpdateTimer == NULL) {
        ERROR_PRINT("Failed to create throttle timer\n");
        return HAL_ERROR;
    }

    init.maxStateNum = STATE_ANY;
    init.maxEventNum = EV_ANY;
    init.sizeofEventEnumType = sizeof(VCU_Events_t);
    init.ST_ANY = STATE_ANY;
    init.EV_ANY = EV_ANY;
    init.transitions = transitions;
    init.transitionTableLength = TRANS_COUNT(transitions);
    init.eventQueueLength = 5;
    init.watchdogTaskId = 1;
    if (fsmInit(STATE_Self_Check, &init, &fsmHandle) != HAL_OK) {
        ERROR_PRINT("Failed to init drive by wire fsm\n");
        return HAL_ERROR;
    }

    if (registerTaskToWatch(1, 5, true, &fsmHandle) != HAL_OK) {
        return HAL_ERROR;
    }

    DEBUG_PRINT("Init drive by wire\n");
    return HAL_OK;
}

void driveByWireTask(void *pvParameters)
{
    // Pre send EV_INIT to kick off self tests
    startDriveByWire();

    if (canStart(&CAN_HANDLE) != HAL_OK) {
        Error_Handler();
    }

    fsmTaskFunction(&fsmHandle);

    for(;;); // Shouldn't reach here
}

HAL_StatusTypeDef startDriveByWire()
{
    return fsmSendEvent(&fsmHandle, EV_Init, portMAX_DELAY /* timeout */); // Force run of self checks
}

uint32_t runSelftTests(uint32_t event)
{
    // TODO: Run some tests
    
    if (brakeAndThrottleStart() != HAL_OK)
    {
        return EM_Fault(EV_Throttle_Failure);
    }

    return STATE_EM_Disable;
}

uint32_t EM_Enable(uint32_t event)
{
    bool bpsState = checkBPSState();
    bool hvEnable = getHvEnableState();
    float brakePressure = getBrakePressurePercent();

    if (!bpsState) {
        DEBUG_PRINT("Failed to em enable, bps fault\n");
        sendDTC_WARNING_EM_ENABLE_FAILED(0);
        return STATE_EM_Disable;
    }
    if (!(brakePressure > MIN_BRAKE_PRESSURE)) {
        DEBUG_PRINT("Failed to em enable, brake pressure low (%f)\n", brakePressure);
        sendDTC_WARNING_EM_ENABLE_FAILED(1);
        return STATE_EM_Disable;
    }
    if (!(throttleIsZero())) {
        DEBUG_PRINT("Failed to em enable, non-zero throttle\n");
        sendDTC_WARNING_EM_ENABLE_FAILED(2);
        return STATE_EM_Disable;
    }
    if (!isBrakePressed()) {
        DEBUG_PRINT("Failed to em enable, brake is not pressed\n");
        sendDTC_WARNING_EM_ENABLE_FAILED(3);
        return STATE_EM_Disable;
    }
    if (!hvEnable) {
        sendDTC_WARNING_EM_ENABLE_FAILED(4);
        DEBUG_PRINT("Failed to em enable, not HV enabled\n");
        return STATE_EM_Disable;
    }

    DEBUG_PRINT("Trans to em enable\n");
    if (MotorStart() != HAL_OK) {
        ERROR_PRINT("Failed to turn on motors\n");
        sendDTC_FATAL_EM_ENABLE_FAILED(5);
        return STATE_Failure_Fatal;
    }

    return STATE_EM_Enable;
}

uint32_t EM_Fault(uint32_t event)
{
    int newState = STATE_Failure_Fatal;
    int currentState = fsmGetState(&fsmHandle);

    if (fsmGetState(&fsmHandle) == STATE_Failure_Fatal) {
        DEBUG_PRINT("EM Fault, already in fatal failure state\n");
        return STATE_Failure_Fatal;
    }

    switch (event) {
        case EV_Bps_Fail:
            {
                sendDTC_CRITICAL_BPS_FAIL();
                DEBUG_PRINT("Bps failed, trans to fatal\n");
                newState = STATE_Failure_Fatal;
            }
            break;
        case EV_Brake_Pressure_Fault:
            {
                sendDTC_CRITICAL_Brake_Pressure_FAIL();
                DEBUG_PRINT("Brake pressure fault, trans to fatal failure\n");
                newState = STATE_Failure_Fatal;
            }
            break;
        case EV_DCU_Can_Timeout:
            {
                sendDTC_FATAL_DCU_CAN_Timeout();
                DEBUG_PRINT("DCU CAN Timeout, trans to fatal failure\n");
                newState = STATE_Failure_Fatal;
            }
            break;
        case EV_Throttle_Failure:
            {
                sendDTC_CRITICAL_Throtte_Failure();
                DEBUG_PRINT("Throttle read failure, trans to fatal failure\n");
                newState = STATE_Failure_Fatal;
            }
            break;
        case EV_Hv_Disable:
            {
                if (currentState == STATE_EM_Disable) {
                    DEBUG_PRINT("HV Disable, staying in EM Disabled state\n");
                } else {
                    DEBUG_PRINT("HV Disable, trans to EM Disabled\n");
                }
                newState = STATE_EM_Disable;
            }
            break;
        case EV_EM_Toggle:
            {
                DEBUG_PRINT("EM Toggle, trans to EM Disabled\n");
                newState = STATE_EM_Disable;
            }
            break;
        case EV_Fatal:
            {
                DEBUG_PRINT("Received fatal event, trans to fatal failure\n");
                sendDTC_FATAL_VCU_F7_ERROR();
                newState = STATE_Failure_Fatal;
            }
            break;
        default:
            {
                sendDTC_FATAL_VCU_F7_ERROR();
                DEBUG_PRINT("EM Enabled, unknown event %lu\n", event);
            }
            break;
    }

    if (fsmGetState(&fsmHandle) == STATE_EM_Enable) {
        if (MotorStop() != HAL_OK) {
            ERROR_PRINT("Failed to stop motors\n");
            newState = STATE_Failure_Fatal;
        }
    }

    return newState;
}

uint32_t EM_Update_Throttle(uint32_t event)
{
    if (fsmGetState(&fsmHandle) != STATE_EM_Enable) {
        ERROR_PRINT("Shouldn't be updating throttle when not in em enabled state\n");
        return fsmGetState(&fsmHandle);
    }
    // TODO: Update Throttle
    DEBUG_PRINT("Updating throttle\n");
    if (outputThrottle() != HAL_OK) {
        // TODO: Turn of motors
        ERROR_PRINT("Throttle update failed, trans to fatal\n");
        if (MotorStop() != HAL_OK) {
            ERROR_PRINT("Failed to stop motors\n");
        }
        return STATE_Failure_Fatal;
    }

    return STATE_EM_Enable;
}

uint32_t DefaultTransition(uint32_t event)
{
    ERROR_PRINT("No transition function registered for state %lu, event %lu\n",
                fsmGetState(&fsmHandle), event);

    sendDTC_FATAL_VCU_F7_ERROR();
    if (MotorStop() != HAL_OK) {
        ERROR_PRINT("Failed to stop motors\n");
    }
    return STATE_Failure_Fatal;
}

void throttleTimerCallback(TimerHandle_t timer)
{
    if (fsmSendEvent(&fsmHandle, EV_Throttle_Poll, 0) != HAL_OK) {
        ERROR_PRINT("Failed to process throttle poll event\n");
    }
}

HAL_StatusTypeDef turnOnMotorControllers() {
    uint32_t dbwTaskNotifications;

    // Request PDU to turn on motor controllers
    EM_Power_State_Request = EM_Power_State_Request_On;
    sendCAN_VCU_EM_Power_State_Request();

    // Wait for PDU to turn on MCs
    xTaskNotifyWait( 0x00,      /* Don't clear any notification bits on entry. */
                     UINT32_MAX, /* Reset the notification value to 0 on exit. */
                     &dbwTaskNotifications, /* Notified value pass out in
                                          dbwTaskNotifications. */
                     pdMS_TO_TICKS(MOTOR_CONTROLLER_PDU_PowerOnOff_Timeout_MS));  /* Timeout */

    if (dbwTaskNotifications & (1<<NTFY_MCs_ON)) {
        DEBUG_PRINT("PDU has turned on MCs\n");
    } else {
        ERROR_PRINT("Got unexpected notification 0x%lX\n", dbwTaskNotifications);
        return HAL_ERROR;
    }

    return HAL_OK;
}

HAL_StatusTypeDef turnOffMotorControllers() {
    uint32_t dbwTaskNotifications;

    // Request PDU to turn on motor controllers
    EM_Power_State_Request = EM_Power_State_Request_Off;
    sendCAN_VCU_EM_Power_State_Request();

    // Wait for PDU to turn on MCs
    xTaskNotifyWait( 0x00,      /* Don't clear any notification bits on entry. */
                     UINT32_MAX, /* Reset the notification value to 0 on exit. */
                     &dbwTaskNotifications, /* Notified value pass out in
                                          dbwTaskNotifications. */
                     pdMS_TO_TICKS(MOTOR_CONTROLLER_PDU_PowerOnOff_Timeout_MS));  /* Timeout */

    if (dbwTaskNotifications & (1<<NTFY_MCs_OFF)) {
        DEBUG_PRINT("PDU has turned off MCs\n");
    } else {
        ERROR_PRINT("Got unexpected notification 0x%lX\n", dbwTaskNotifications);
        return HAL_ERROR;
    }

    return HAL_OK;
}

HAL_StatusTypeDef MotorStart()
{
    DEBUG_PRINT("Starting motors\n");
    if (turnOnMotorControllers() != HAL_OK) {
        return HAL_ERROR;
    }

    if (xTimerStart(throttleUpdateTimer, 100) != pdPASS) {
        ERROR_PRINT("Failed to start throttle update timer\n");
        return HAL_ERROR;
    }

    return HAL_OK;
}

HAL_StatusTypeDef MotorStop()
{
    DEBUG_PRINT("Stopping motors\n");

    if (xTimerStop(throttleUpdateTimer, 100) != pdPASS) {
        ERROR_PRINT("Failed to stop throttle update timer\n");
        return HAL_ERROR;
    }

    if (turnOffMotorControllers() != HAL_OK) {
        return HAL_ERROR;
    }

    return HAL_OK;
}
