#include "canReceive.h"

#include "userCan.h"
#include "bsp.h"
#include "debug.h"

#include "drive_by_wire.h"

#include "VCU_F7_can.h"

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"

/*
 * External Board Statuses:
 * Variables for keeping track of external board statuses that get updated by
 * Can messages
 */
volatile bool motorControllersStatus = false;
uint32_t lastBrakeValReceiveTimeTicks = 0;

/*
 * Functions to get external board status
 */
bool getHvEnableState()
{
    return HV_Power_State == HV_Power_State_On;
}

bool getMotorControllersStatus()
{
    return motorControllersStatus;
}

extern osThreadId driveByWireHandle;

void CAN_Msg_DCU_buttonEvents_Callback()
{
    if (ButtonEMEnabled) {
        fsmSendEventISR(&fsmHandle, EV_EM_Toggle);
    }
    // For now, ignore HV Enable button, as we really want to wait for BMU to
    // complete HV Enable
}


void CAN_Msg_PDU_ChannelStatus_Callback()
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    
    if (!motorControllersStatus && StatusPowerMCLeft == StatusPowerMCLeft_CHANNEL_ON &&
        StatusPowerMCRight == StatusPowerMCRight_CHANNEL_ON) {
        xTaskNotifyFromISR( driveByWireHandle,
                            (1<<NTFY_MCs_ON),
                            eSetBits,
                            &xHigherPriorityTaskWoken );
        motorControllersStatus = true;
    } else if (motorControllersStatus) {
        // Only send a notification if MCs turned off if MCs were already ON
        xTaskNotifyFromISR( driveByWireHandle,
                            (1<<NTFY_MCs_OFF),
                            eSetBits,
                            &xHigherPriorityTaskWoken );
        motorControllersStatus = false;
    }

    portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
}

void DTC_Fatal_Callback(BoardIDs board)
{
    fsmSendEventUrgentISR(&fsmHandle, EV_Fatal);
}

void CAN_Msg_BMU_HV_Power_State_Callback() {
    DEBUG_PRINT_ISR("Receive hv power state\n");
    if (HV_Power_State != HV_Power_State_On) {
        fsmSendEventISR(&fsmHandle, EV_Hv_Disable);
    }
}

void CAN_Msg_BMU_BrakePedalValue_Callback()
{
    lastBrakeValReceiveTimeTicks = xTaskGetTickCount();
}
