BINARY_BASE_NAME=VCU
BOARD_NAME=VCU_F7

COMMON_LIB_SRC = userCan.c debug.c state_machine.c CRC_CALC.c FreeRTOS_CLI.c freertos_openocd_hack.c watchdog.c canHeartbeat.c generalErrorHandler.c 
COMMON_F7_LIB_SRC = userCanF7.c

# one of NUCLEO_F7, F7
BOARD_TYPE ?= F7
ifeq ($(BOARD_TYPE), F7)
BOARD_VERSION ?= 2
else
BOARD_VERSION ?= 1
endif

CUBE_F7_VERSION_1_MAKEFILE_PATH= Cube-F7-Src/2018_VCU/2018_VCU/
CUBE_F7_VERSION_2_MAKEFILE_PATH= Cube-F7-Src-respin/2018_VCU/2018_VCU/
CUBE_NUCLEO_MAKEFILE_PATH = Cube-Nucleo-Src/CanTest/

include common-all/tail.mk
