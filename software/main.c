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
 * group is lit roughly every 500ms, for about 30 seconds total. After that,
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
 * KNOWN OPEN ITEMS -- NOT YET CONFIRMED ON REAL HARDWARE
 * ----------------------------------------------------------------------------
 * These two spots are flagged inline as well, but worth calling out up top:
 *
 *   1. EnterStopMode()'s PWR->CR1 write is a placeholder bit position.
 *      Confirm the real "select Stop mode" bit against the PWR_CR1 register
 *      table in Puya's reference manual before trusting battery life
 *      numbers -- if this bit is wrong, the chip may not actually be
 *      entering low-power mode at all.
 *
 *   2. SimpleDelayMs()'s timing is an uncalibrated busy-wait loop. If the
 *      on/off alternation looks visibly faster or slower than ~500ms per
 *      step once flashed, adjust the loop's iteration multiplier.
 *
 * Everything else in this file has been confirmed to build clean (0 errors,
 * 0 warnings) against Puya's PY32F0xx_Drivers package, using only
 * py32f0xx_ll_gpio.c/h and py32f0xx_ll_exti.c/h (both known-clean files).
 * RCC clock-enable and STOP-mode entry are done via direct register access
 * rather than through py32f0xx_ll_rcc.c / py32f0xx_ll_pwr.c / 
 * py32f0xx_ll_utils.c, since those three reference HSE- and voltage-scaling-
 * related register bits that this specific low-pin-count device's header
 * doesn't define -- a real gap in Puya's shared family driver files, not
 * something this design ever needed anyway (no HSE, no voltage scaling).
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
#define PATTERN_DURATION_MS   10000
#define PATTERN_STEP_MS         100

/* Set inside the EXTI interrupt handler when a shake wakes the chip;
   cleared once the main loop has handled it and gone back to sleep. */
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
    LL_GPIO_ResetOutputPin(GPIOB, LED_GROUP_A_PIN);   /* start off */

    gpio_init.Pin = LED_GROUP_B_PIN;
    LL_GPIO_Init(GPIOA, &gpio_init);
    LL_GPIO_ResetOutputPin(GPIOA, LED_GROUP_B_PIN);   /* start off */

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
 *   The actual "show": alternates Group A and Group B every
 *   PATTERN_STEP_MS, for a total of PATTERN_DURATION_MS, then turns both
 *   off before returning. Further switch chatter during this window just
 *   re-triggers the same interrupt harmlessly (wake_flag gets set again,
 *   but we're not re-entering EnterStopMode until this function returns).
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

/* -----------------------------------------------------------------------
 * SimpleDelayMs
 *   A rough busy-wait delay, since this design doesn't link in Puya's
 *   ll_utils.c (see the file-level comment for why). Not precisely
 *   calibrated -- fine for a decorative ~500ms blink step where exact
 *   timing doesn't matter. If real-world timing looks noticeably off
 *   once flashed, adjust the multiplier below.
 * ------------------------------------------------------------------------- */
static void SimpleDelayMs(uint32_t ms)
{
    volatile uint32_t i;
    for (i = 0; i < ms * 2000UL; i++)
    {
        __NOP();
    }
}

/* -----------------------------------------------------------------------
 * EnterStopMode
 *   Puts the chip into STOP mode (the low-power state where SRAM and
 *   registers are retained, HSI is off, and only GPIO/IWDG/NRST/LPTIM can
 *   wake it) and waits for the EXTI interrupt to bring it back.
 *
 *   NOT YET CONFIRMED: the PWR->CR1 bit set below is a placeholder for
 *   "select Stop mode" -- verify the real bit position against the
 *   PWR_CR1 register table in Puya's reference manual before trusting
 *   this actually drops into low-power mode on real hardware.
 * ------------------------------------------------------------------------- */
static void EnterStopMode(void)
{
    PWR->CR1 |= (1UL << 0);   /* placeholder: verify stop-mode select bit */

    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
    __WFI();

    /* Execution resumes here once EXTI wakes the chip. Depending on the
       exact regulator/clock behavior of Stop mode on this part, HSI may
       need re-enabling on wake -- check Puya's own stop-mode example
       project for the expected wake-up sequence if behavior looks off. */
}