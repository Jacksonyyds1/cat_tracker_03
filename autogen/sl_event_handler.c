#include "sl_event_handler.h"

#include "rsi_nvic_priorities_config.h"
#include "sl_si91x_clock_manager.h"
#include "sli_siwx917_soc.h"
#include "rsi_board.h"
#include "rsi_debug.h"
#include "sl_i2c_instances.h"
#include "sl_si91x_button_instances.h"
#include "sl_si91x_led_instances.h"
#include "cmsis_os2.h"

void sli_driver_permanent_allocation(void)
{
}

void sli_service_permanent_allocation(void)
{
}

void sli_stack_permanent_allocation(void)
{
}

void sli_internal_permanent_allocation(void)
{
}

void sl_platform_init(void)
{
  sl_si91x_device_init_nvic();
  sl_si91x_clock_manager_init();
  sli_si91x_platform_init();
  RSI_Board_Init();
  DEBUGINIT();
}

void sli_internal_init_early(void)
{
}

void sl_kernel_start(void)
{
  osKernelStart();
}

void sl_driver_init(void)
{
  sl_i2c_init_instances();
  button_init_instances();
  led_init_instances();
}

void sl_service_init(void)
{
}

void sl_stack_init(void)
{
}

void sl_internal_app_init(void)
{
}

