/* =============================================================================
 * main.c - Shake-triggered alternating LED Christmas ornament
 * Target:  Puya PY32F002BL15S7TU (SOP8), Cortex-M0+, 24KB flash / 3KB RAM
 * Power:   CR2032 coin cell
 * =============================================================================
 *
 * WHAT THIS SYSTEM DOES
 * ----------------------------------------------------------------------------
 * The board sits in low-power STOP mode almost all the time, drawing close
 * to nothing from the battery. A mechanical vibration switch (HX-0805-C2) on
 * pin PB0 chatters between high and low whenever the ornament is bumped or
 * shaken. That level change wakes the chip via an EXTI (external interrupt)
 * on line 0.
 *
 * Once awake, the firmware alternates two groups of LEDs -- Group A on PB1,
 * Group B on PA7, each driven through its own N-MOSFET -- switching which
 * group is lit roughly every 250ms, for about 5 seconds total. After that,
 * both groups turn off and the chip drops straight back into STOP mode to
 * wait for the next shake.
 *
 * Because the chip spends the overwhelming majority of its life asleep, the
 * battery life of this design is dominated by STOP-mode current, not by how
 * often it's shaken or how bright the LEDs are.
 *
 * PIN MAP
 * ----------------------------------------------------------------------------
 *   PB0  - vibration switch input (external 1-10M pull resistor on the PCB
 *          does the pulling; this pin is configured as a plain floating
 *          input, not using the MCU's internal pull)
 *   PB1  - gate of the N-MOSFET driving LED group A
 *   PA7  - gate of the N-MOSFET driving LED group B
 *   PA2  - SWCLK (left alone here; used only for programming/debug)
 *   PB6  - SWDIO (left alone here; used only for programming/debug)
 *
 * TIMING
 * ----------------------------------------------------------------------------
 * SysTick derives delays from the current core clock. Actual wall-clock
 * timing is subject to the internal oscillator's accuracy.
 * ===========================================================================
 */

#include "py32f0xx.h"
#include "py32f0xx_ll_gpio.h"
#include "py32f0xx_ll_exti.h"

/* ---- Pin assignments -------------------------------------------------- */
#define LED_GROUP_A_PIN   LL_GPIO_PIN_1   /* PB1 */
#define LED_GROUP_B_PIN   LL_GPIO_PIN_7   /* PA7 */
#define SWITCH_PIN        LL_GPIO_PIN_0   /* PB0 */

/* ---- Pattern timing ----------------------------------------------------
 * PATTERN_DURATION_MS: how long the LED show runs after being woken.
 * PATTERN_STEP_MS:     how long each group stays lit before swapping.
 * ------------------------------------------------------------------------ */
#define PATTERN_DURATION_MS    5000
#define PATTERN_STEP_MS         250

/* Set by EXTI when a shake wakes the chip. */
static volatile uint8_t wake_flag = 0;

static void GPIO_Config(void);
static void EXTI_Config(void);
static void RunLedPattern(void);
static void EnterStopMode(void);
static void SimpleDelayMs(uint32_t ms);

int main(void)
{
    /* SystemInit() has already run before main() via the startup file,
       so the system clock is already configured (HSI, no HSE needed). */

    GPIO_Config();
    EXTI_Config();

    while (1)
    {
        EnterStopMode();   /* blocks here, asleep, until a shake wakes us */

        if (wake_flag)
        {
            wake_flag = 0;
            RunLedPattern();
            wake_flag = 0;  /* Ignore switch chatter during the pattern. */
        }
    }
}

/* -----------------------------------------------------------------------
 * GPIO_Config
 *   Sets up every pin this design touches:
 *     - PB1, PA7 as push-pull outputs, both driven low (LEDs off) at boot
 *     - PB0 as a floating input (external resistor network on the board
 *       does the pull-up, so no internal pull is enabled here)
 * ------------------------------------------------------------------------- */
static void GPIO_Config(void)
{
    /* Enable the GPIOA and GPIOB peripheral clocks directly.
       RCC->IOPENR bit 0 = GPIOA, bit 1 = GPIOB on Puya's ST-mirrored
       register layout. (GPIOC isn't needed since this design only uses
       pins on ports A and B.) */
    RCC->IOPENR |= (1UL << 0) | (1UL << 1);

    LL_GPIO_InitTypeDef gpio_init;
    LL_GPIO_StructInit(&gpio_init);

    gpio_init.Mode = LL_GPIO_MODE_OUTPUT;
    gpio_init.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    gpio_init.Speed = LL_GPIO_SPEED_FREQ_LOW;

    gpio_init.Pin = LED_GROUP_A_PIN;
    LL_GPIO_Init(GPIOB, &gpio_init);
    LL_GPIO_ResetOutputPin(GPIOB, LED_GROUP_A_PIN); /* LEDs off */

    gpio_init.Pin = LED_GROUP_B_PIN;
    LL_GPIO_Init(GPIOA, &gpio_init);
    LL_GPIO_ResetOutputPin(GPIOA, LED_GROUP_B_PIN); /* LEDs off */

    LL_GPIO_InitTypeDef sw_init;
    LL_GPIO_StructInit(&sw_init);
    sw_init.Pin = SWITCH_PIN;
    sw_init.Mode = LL_GPIO_MODE_INPUT;
    sw_init.Pull = LL_GPIO_PULL_NO;
    LL_GPIO_Init(GPIOB, &sw_init);
}

/* -----------------------------------------------------------------------
 * EXTI_Config
 *   Routes PB0 to EXTI line 0 and arms it to interrupt on either a rising
 *   or falling edge, since a real shake produces a chattery burst of both
 *   directions rather than one clean transition.
 * ------------------------------------------------------------------------- */
static void EXTI_Config(void)
{
    LL_EXTI_InitTypeDef exti_init;
    LL_EXTI_StructInit(&exti_init);

    exti_init.Line = LL_EXTI_LINE_0;
    exti_init.LineCommand = ENABLE;
    exti_init.Mode = LL_EXTI_MODE_IT;
    exti_init.Trigger = LL_EXTI_TRIGGER_RISING_FALLING;
    LL_EXTI_Init(&exti_init);

    /* Map EXTI line 0 to GPIO port B, so it watches PB0 specifically
       rather than PA0 or PC0 (all three share line 0 on this family). */
    LL_EXTI_SetEXTISource(LL_EXTI_CONFIG_PORTB, LL_EXTI_CONFIG_LINE0);

    NVIC_EnableIRQ(EXTI0_1_IRQn);
    NVIC_SetPriority(EXTI0_1_IRQn, 0);
}

/* -----------------------------------------------------------------------
 * EXTI0_1_IRQHandler
 *   Fires on every edge the vibration switch produces. Just clears the
 *   flag and sets wake_flag -- the actual LED pattern runs from the main
 *   loop, not from inside the interrupt, to keep the ISR itself short.
 * ------------------------------------------------------------------------- */
void EXTI0_1_IRQHandler(void)
{
    if (LL_EXTI_IsActiveFlag(LL_EXTI_LINE_0))
    {
        LL_EXTI_ClearFlag(LL_EXTI_LINE_0);
        wake_flag = 1;
    }
}

/* -----------------------------------------------------------------------
 * RunLedPattern
 *   Alternates the LED groups at PATTERN_STEP_MS intervals for
 *   PATTERN_DURATION_MS, then turns both groups off.
 * ------------------------------------------------------------------------- */
static void RunLedPattern(void)
{
    uint32_t elapsed = 0;
    uint8_t group_a_on = 1;

    while (elapsed < PATTERN_DURATION_MS)
    {
        if (group_a_on)
        {
            LL_GPIO_SetOutputPin(GPIOB, LED_GROUP_A_PIN);
            LL_GPIO_ResetOutputPin(GPIOA, LED_GROUP_B_PIN);
        }
        else
        {
            LL_GPIO_ResetOutputPin(GPIOB, LED_GROUP_A_PIN);
            LL_GPIO_SetOutputPin(GPIOA, LED_GROUP_B_PIN);
        }
        group_a_on = !group_a_on;

        SimpleDelayMs(PATTERN_STEP_MS);
        elapsed += PATTERN_STEP_MS;
    }

    /* Both groups off before returning to sleep */
    LL_GPIO_ResetOutputPin(GPIOB, LED_GROUP_A_PIN);
    LL_GPIO_ResetOutputPin(GPIOA, LED_GROUP_B_PIN);
}

/* Wait for the requested number of milliseconds using the core clock. */
static void SimpleDelayMs(uint32_t ms)
{
    if (ms == 0U)
    {
        return;
    }

    SysTick->CTRL = 0U;
    SystemCoreClockUpdate();
    SysTick->LOAD = (SystemCoreClock / 1000UL) - 1UL;
    SysTick->VAL = 0U;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;

    while (ms > 0U)
    {
        while ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) == 0U)
        {
        }
        --ms;
    }

    SysTick->CTRL = 0U;
}

/* Configure the clock and enter STOP mode until an interrupt wakes the MCU. */
static void EnterStopMode(void)
{
    /* The reference manual says SYSCLK should be HSI before STOP entry.
       HSI is automatically selected again when STOP is exited, but doing
       the switch explicitly here also handles the case where another part
       of the application has changed SYSCLK. */
    RCC->CR |= (1UL << 8);                 /* HSION */
    while ((RCC->CR & (1UL << 10)) == 0)   /* HSIRDY */
    {
    }

    /* Select HSISYS (SW = 000) and remove any AHB prescaler (HPRE = 0000).
       The reference manual specifies HPRE=0 before STOP to avoid extra
       clock cycles during wake-up. */
    RCC->CFGR &= ~((0xFUL << 8) | 0x7UL);
    while ((RCC->CFGR & (0x7UL << 3)) != 0)
    {
        /* Wait until the hardware reports HSISYS as the active SYSCLK. */
    }

    /* LPR = 01: STOP mode powered by the low-power regulator.
       Preserve FLS_SLPTIME, HSION_CTRL, SRAM_RETV and reserved bits. */
    PWR->CR1 = (PWR->CR1 & ~(3UL << 14)) | (1UL << 14);

    /* Keep the wake check and sleep instruction atomic with respect to the
       ISR. A pending EXTI edge wakes WFI while masked, then is serviced when
       interrupts are re-enabled. Do not clear pending state here: it may be
       the shake that should wake the ornament. */
    __disable_irq();
    if (wake_flag || (EXTI->PR & (1UL << 0)) != 0)
    {
        __enable_irq();
        return;
    }

    /* Enter Cortex-M0+ deep sleep, which the PY32 power controller turns
       into STOP mode with the PWR_CR1 settings above. */
    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
    __DSB();
    __WFI();
    __ISB();
    __enable_irq();

    /* STOP exit automatically selects HSI as SYSCLK. Nothing else is
       required here for this application. */
}
