#include <linux/delay.h>
#include <linux/hardirq.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include <asm/gpio.h>

#include "./indigo-gpioperiph.h"

#define PRINT(log_flag, format, args...)		\
	printk(log_flag format "\n", ## args)

#define TRACE_ENTRY() do {                              \
                PRINT(KERN_INFO, "ENTRY %s", __func__); \
        } while (0)

#define TRACE_EXIT() do {                               \
                PRINT(KERN_INFO, "LEAVE %s", __func__); \
        } while (0)

#define TRACE_EXIT_RES(res) do {                                        \
                PRINT(KERN_INFO, "LEAVE %s result %d", __func__, res);  \
        } while (0)

#define TRACE_STEP(step_name, message) do {				\
		PRINT(KERN_INFO, "function %s operation step %s message %s", \
			__func__, step_name, message);			\
	} while (0)

int indigo_gpioperiph_get_pin_by_function(const struct gpio_peripheral *periph,
	enum indigo_pin_function_t function)
{
	int i;

	BUG_ON(periph == NULL);

	for (i = 0; i < INDIGO_MAX_GPIOPERIPH_PIN_COUNT; i++) {
		if (periph->pins[i].function == function)
			return i;

		if (periph->pins[i].description == NULL)
			break;
	}

	/* maybe this pin is not that crucial */
	return INDIGO_NO_PIN;
}

/* kernel panic if pin's not found, thus no error handling need in caller */
int indigo_gpioperiph_get_mandatory_pin_by_function(const struct gpio_peripheral *periph,
						enum indigo_pin_function_t function,
						bool mandatory)
{
	int pin;

	BUG_ON(periph == NULL);

	indigo_gpioperiph_get_pin_by_function(periph, function);

	if (mandatory && pin == INDIGO_NO_PIN) {
		BUG();
		panic("couldn't find function %d for periph %s\n",
			function, periph->description);
	}

	return pin;
}

/* смысл, в основном, в том, чтобы дополнить разницу
 * между общими функциями gpio_* и атмеловские at91_*
 */
int indigo_request_pin(const struct indigo_periph_pin *pin)
{
	int result;

	BUG_ON(pin == NULL);
	/* этот gpio_request_one поймёт GPIOF_DIR_(IN,OUT)
	 * GPIOF_INIT_(LOW_HIGH)
	 */
	result = gpio_request(pin->pin_no, pin->description);
	if (result) {
		printk(KERN_ERR "failed to request pin %s\n", pin->description);
		BUG();
		panic("failed to request pin %s\n", pin->description);
		goto done;
	}

	at91_set_GPIO_periph(pin->pin_no, pin->flags & GPIOF_PULLUP);

	if (pin->flags & GPIOF_DIR_IN) {
		/* input pin */
		at91_set_gpio_input(pin->pin_no,
				(pin->flags & GPIOF_PULLUP) != 0);
		at91_set_deglitch(pin->pin_no,
				(pin->flags & GPIOF_DEGLITCH) != 0);
	} else {
		/* output pin */
		at91_set_gpio_output(pin->pin_no,
				(pin->flags & GPIOF_INIT_HIGH) != 0);
	}
done:
	return result;
}

static int indigo_configure_pin(
	const struct gpio_peripheral *periph,
	enum indigo_pin_function_t function,
	bool mandatory)
{
	int pin;

	BUG_ON(periph == NULL);

	pin = indigo_gpioperiph_get_mandatory_pin_by_function(periph,
							function, mandatory);
	if ((pin == INDIGO_NO_PIN) && !mandatory)
		goto done;

	indigo_request_pin(&periph->pins[pin]);

done:
	return pin;
}

/*
 * Invert @value arg if INDIGO_ACTIVE_LOW flag is set for this pin
 *
 * This function answers if this value is active for current pin.
 */
static int indigo_pin_active_value(const struct indigo_periph_pin *pin,
	int value)
{
	/*
	 * active low == 1 ^ status = 1 -> 0
	 * active low == 1 ^ status = 0 -> 1
	 * active low == 0 ^ status = 1 -> 1
	 * active low == 0 ^ status = 0 -> 0
	 */
	return ((pin->flags & GPIOF_ACTIVE_LOW) != 0) ^ value;
}

int indigo_configure_general_pins(struct gpio_peripheral *periph)
{
	int i;

	BUG_ON(periph == NULL);

	for (i = 0; i < INDIGO_MAX_GPIOPERIPH_PIN_COUNT; i++) {
		if (periph->pins[i].description == NULL)
			break;

		/* остальные пины ушли в инициализации девайсов */
		if (periph->pins[i].function != INDIGO_FUNCTION_NO_FUNCTION)
			continue;

		indigo_request_pin(&periph->pins[i]);
	}
}
EXPORT_SYMBOL(indigo_configure_general_pins);

/**
 * only valid for output pins
 *
 * @value is corrected by @function pin flags
 */
static void indigo_gpioperiph_set_output(const struct gpio_peripheral *periph,
					enum indigo_pin_function_t function,
					int value,
					bool mandatory)
{
	int pin;

	pin = indigo_gpioperiph_get_mandatory_pin_by_function(periph, function, mandatory);

	if (pin == INDIGO_NO_PIN) {
		printk(KERN_INFO "non-mandatory pin for function %d not found\n", function);
		goto done;
	}

	/* otherwise, there'll be panic */

	if ((periph->pins[pin].flags & GPIOF_DIR_IN) != 0) {
		printk(KERN_ERR "tried to output to input pin %d\n", pin);
		BUG();
		goto done;
	}

	gpio_set_value(periph->pins[pin].pin_no,
			indigo_pin_active_value(&periph->pins[pin], value));

done:
	return;
}

/**
 * @step_count == ARRAY_SIZE(steps)
 *
 * @INDIGO_FUNCTION_STATUS as pin kind is handled by timeout
 */
static int indigo_gpio_perform_sequence(struct indigo_gpio_sequence_step **steps,
	int step_count)
{
	int i;
	int result;
	int status;
	int timeout;

	for (i = 0; i < step_count; i++) {
		TRACE_STEP(steps[i]->step_no, steps[i]->step_desc);

		/* last step indicator */
		if (steps[i]->periph == NULL)
			goto out;

		/* function is not mandatory when it's just a timeout waiting */
		if (steps[i]->function != INDIGO_FUNCTION_NO_FUNCTION &&
			steps[i]->function != INDIGO_FUNCTION_STATUS) {

			indigo_gpioperiph_set_output(steps[i]->periph,
				steps[i]->function, steps[i]->value, steps[i]->mandatory);
		}

		if (steps[i]->sleep_ms != 0)
			msleep(steps[i]->sleep_ms);

		/* only timeout on status function available */
		if (steps[i]->timeout_ms != 0 && steps[i]->function == INDIGO_FUNCTION_STATUS) {
			status = steps[i]->periph->status(steps[i]->periph);
			/* wait for given status value if INDIGO_FUNCTION_STATUS happened*/
			while (timeout < steps[i]->timeout_ms && !steps[i]->value) {
				msleep(500);
				timeout = timeout + 500;
				status = steps[i]->periph->status(steps[i]->periph);
			}
			result = !status;
		}
	}

out:
	TRACE_EXIT();
	return result;
}

/* general GSM routines */

/**
 * Returns status pin value w/o interpretation
 */
static int gsm_generic_status(const struct gpio_peripheral *periph)
{
	int status_pin = indigo_gpioperiph_get_pin_by_function(periph,
		INDIGO_FUNCTION_STATUS);

	return indigo_pin_active_value(
		&periph->pins[status_pin],
		gpio_get_value(periph->pins[status_pin].pin_no));
}

static void indigo_peripheral_create_command(struct gpio_peripheral *peripheral,
	enum indigo_gpioperiph_command_t command);

static irqreturn_t keep_turned_on_handler_irq(int irq, void *dev)
{
	struct gpio_peripheral *device = (struct gpio_peripheral *) dev;

	/*
	 * schedule a check of device status at the end of command queue
	 * and turn it on if status is 0 that time
	 */
	indigo_peripheral_create_command(device, INDIGO_COMMAND_CHECK_AND_POWER_ON);

	return IRQ_HANDLED;
}

/**
 * Configure status pin for given interrupt handler, a pwrkey pin
 * and power pin if one's available
 *
 * All supported modems have at least two functional pins --
 * pwr_key and status pin. Our schematics has a power pin to
 * physically turn off the device.
 */
static int gsm_generic_simcom_setup(struct gpio_peripheral *periph,
	irq_handler_t status_pin_handler)
{
	int result;
	int status;

	TRACE_ENTRY();

	/* we will not return from there if configuration has failed */

	status = indigo_configure_pin(periph, INDIGO_FUNCTION_STATUS, /* mandatory */ true);

	if (request_irq(periph->pins[status].pin_no, status_pin_handler, 0,
			periph->pins[status].description, (void *) periph)) {

		printk(KERN_ERR "can not request irq for status pin\n");
		BUG(); /* not fatal */
		result = -ENODEV;
		goto done;
        }

	indigo_configure_pin(periph, INDIGO_FUNCTION_PWRKEY, /* mandatory */ true);

	/* it doesn't really matter if it's not found */
	indigo_configure_pin(periph, INDIGO_FUNCTION_POWER, /* mandatory */ false);

	periph->status = gsm_generic_status;

done:
	TRACE_EXIT_RES(result);
	return result;
}

/* FIXME error handling through int result */
static int indigo_generic_reset(const struct gpio_peripheral *periph)
{
	int result;

	TRACE_ENTRY();

	BUG_ON(periph == NULL);

	if (periph->status(periph)) {
		/* option 1. Full restart */
		TRACE_STEP("1", "Full restart");
		TRACE_STEP("1.1", "Power off");

		result = periph->power_off(periph);

		if (result) {
			printk(KERN_ERR "couldn't power off the device");
			goto out;
		}

		TRACE_STEP("1.2", "Power on");
		result = periph->power_on(periph);
		if (result) {
			printk(KERN_ERR "couldn't power on the device");
			goto out;
		}
	} else { /* option 2. Just power on */
		TRACE_STEP("2", "Just power on");
		result = periph->power_on(periph);
		goto out;
	}

out:
	TRACE_EXIT_RES(result);
	return result;
}

static int indigo_check_and_power_on(const struct gpio_peripheral *periph)
{
	int result;

	TRACE_ENTRY();

	BUG_ON(periph == NULL);

	if (!periph->status(periph))
		result = periph->power_on(periph);

	TRACE_EXIT_RES(result);
	return result;
}

/*
 * ------------------------------------------------------
 * ------------------------------------------------------
 */

/*
 *     SimCOM Sim508 GSM Module
 *
 * Sim508 Hardware Definition 2.08
 *
 * FIXME error reporting
 */


/* p.3.4.1.1, figure 3 */
int gsm_sim508_power_on(const struct gpio_peripheral *periph)
{
	int status = 0;

	int timeout = 0;

	struct indigo_gpio_sequence_step steps[] = {
		{"0", "turn on POWER pin if available",
		 periph, INDIGO_FUNCTION_POWER, 0, false, 0, 0},

		{"1", "pwrkey to 1 for 0.5s -- nonstrict, ends at t0",
		 periph, INDIGO_FUNCTION_PWRKEY, 1, true, 500, 0},

		{"2", "pwrkey to 0 for t - t0 > 2s -- strict",
		 periph, INDIGO_FUNCTION_PWRKEY, 0, true, 2100, 0},

		{"3", "pwrkey to 1",
		 periph, INDIGO_FUNCTION_PWRKEY, 1, true, 0, 0},

		/* monitor status pin for value 1 */
		{"4", "wait for status pin to come up",
		 periph, INDIGO_FUNCTION_STATUS, 1, true, 0, 12000},

		{"5", "finally, status pin is 1 when all is ok",
		 NULL, INDIGO_FUNCTION_NO_FUNCTION, 0, true, 0, 0}
	};

	TRACE_ENTRY();

	/* initially, status pin is 0 -- modem is turned off */
	if (periph->status(periph)) {
		printk(KERN_ERR "tried to power on device with status pin 1");
		BUG();
		return -ENODEV;
	}

	indigo_gpio_perform_sequence((struct indigo_gpio_sequence_step **) &steps, ARRAY_SIZE(steps));

	status = periph->status(periph);

	PRINT(KERN_ERR, "status pin is %d", status);

	TRACE_EXIT_RES(!status);
	return !status; /* 0 is OK code, 1 -- error */
}

/* p.3.4.2.1. figure 4 */
int gsm_sim508_power_off(const struct gpio_peripheral *periph)
{
	int status;
	int timeout = 0;

	struct indigo_gpio_sequence_step steps[] = {

		{"1", "pwrkey -> 1 for 500ms",
		 periph, INDIGO_FUNCTION_PWRKEY, 1, true, 500, 0},

		{"2", "pwrkey -> 0 for 2s < t < 1s",
		 periph, INDIGO_FUNCTION_PWRKEY, 0, true, 1500, 0},

		{"3", "pwrkey to 1",
		 periph, INDIGO_FUNCTION_PWRKEY, 1, true, 0, 0},

		/* monitor status pin for value 1 */
		{"4", "wait for 2 to 8 seconds for status pin to come down",
		 periph, INDIGO_FUNCTION_STATUS, 1, true, 0, 10000},

		{"5", "finally, status pin is 0 when all is ok",
		 NULL, INDIGO_FUNCTION_NO_FUNCTION, 0, true, 0, 0}
	};

	TRACE_ENTRY();

	BUG_ON(periph == NULL);

	/* initially, status pin is 1 -- modem is turned on */
	if (!periph->status(periph)) {
		printk(KERN_ERR "tried to power off device with status pin 0");
		BUG();
		return -ENODEV;
	}

	indigo_gpio_perform_sequence((struct indigo_gpio_sequence_step **) &steps, ARRAY_SIZE(steps));

	status = periph->status(periph);

	PRINT(KERN_ERR, "status pin is %d", status);


	TRACE_EXIT_RES(status);
	return status; /* 0 is OK code, 1 -- is error */
}

int gsm_sim508_reset(const struct gpio_peripheral *periph)
{
	return indigo_generic_reset(periph);
}

int gsm_sim508_setup(struct gpio_peripheral *periph)
{
	int result;

	TRACE_ENTRY();

	result = gsm_generic_simcom_setup(periph, keep_turned_on_handler_irq);

	periph->reset = gsm_sim508_reset;
	periph->power_on = gsm_sim508_power_on;
	periph->power_off = gsm_sim508_power_off;
	periph->status = gsm_generic_status;
	periph->check_and_power_on = indigo_check_and_power_on;

	indigo_peripheral_create_command(periph, INDIGO_COMMAND_CHECK_AND_POWER_ON);

	TRACE_EXIT_RES(result);
	return result;
}
EXPORT_SYMBOL(gsm_sim508_setup);
/*
 * ------------------------------------------------------
 * ------------------------------------------------------
 */


/*
 *     SimCOM Sim900D GSM Module (Device 1.0)
 *
 * Sim900D Hardware Design v.1.04
 *
 */

/* figure 9 */
int gsm_sim900D_power_on(const struct gpio_peripheral *periph)
{
	int status = 0;
	int timeout = 0;

	struct indigo_gpio_sequence_step steps[] = {

		{"0", "turn on POWER pin if available",
		 periph, INDIGO_FUNCTION_POWER, 0, false, 0, 0},

		{"1", "pwrkey to 1 for 0.5s -- nonstrict, ends at t0",
		 periph, INDIGO_FUNCTION_PWRKEY, 1, true, 500, 0},

		{"2", "pwrkey to 0 for t - t0 > 1s -- strict",
		 periph, INDIGO_FUNCTION_PWRKEY, 0, true, 1100, 0},

		{"3", "pwrkey to 1",
		 periph, INDIGO_FUNCTION_PWRKEY, 1, true, 0, 0},

		/* monitor status pin for value 1 */
		{"4", "wait for status pin to come up for more than 3.2 seconds after t0",
		 periph, INDIGO_FUNCTION_STATUS, 1, true, 0, 10000},

		{"5", "finally, status pin is 1 when all is ok",
		 NULL, INDIGO_FUNCTION_NO_FUNCTION, 0, true, 0, 0}
	};

	TRACE_ENTRY();

	BUG_ON(periph == NULL);

	/* initially, status pin is 0 -- modem is turned off */
	if (periph->status(periph)) {
		printk(KERN_ERR "tried to power on device with status pin 1");
		BUG();
		return -ENODEV;
	}

	indigo_gpio_perform_sequence((struct indigo_gpio_sequence_step **) &steps, ARRAY_SIZE(steps));

	status = periph->status(periph);
	PRINT(KERN_ERR, "status pin is %d", status);

	TRACE_EXIT_RES(!status);
	return !status; /* 0 is OK code, 1 -- error */
}

/* figure 10 */
int gsm_sim900D_power_off(const struct gpio_peripheral *periph)
{
	int status;
	int timeout = 0;

	struct indigo_gpio_sequence_step steps[] = {
		{"1", "pwrkey -> 0 for 5s < t < 1s",
		 periph, INDIGO_FUNCTION_PWRKEY, 0, true, 2000, 0},

		{"2", "set PWRKEY -> 1",
		 periph, INDIGO_FUNCTION_PWRKEY, 1, true, 50, 0},

		/* monitor status pin for value 0 */
		{"3", "wait for status pin to come down for more than 3.2 seconds after t0",
		 periph, INDIGO_FUNCTION_STATUS, 0, true, 0, 10000},

		{"4", "finally, status pin is 0 when all is ok",
		 NULL, INDIGO_FUNCTION_NO_FUNCTION, 0, true, 0, 0}
	};

	TRACE_ENTRY();

	/* same as sim900 */

	BUG_ON(periph == NULL);

	/* initially, status pin is 1 -- modem is turned on */
	if (!periph->status(periph)) {
		printk(KERN_ERR "tried to power off device with status pin 0");
		BUG();
		return -ENODEV;
	}

	indigo_gpio_perform_sequence((struct indigo_gpio_sequence_step **) &steps, ARRAY_SIZE(steps));

	status = periph->status(periph);

	PRINT(KERN_ERR, "status pin is %d", status);

	TRACE_EXIT_RES(status);
	return status; /* 0 is OK code, 1 -- is error */
}

int gsm_sim900D_reset(const struct gpio_peripheral *periph)
{
	BUG_ON(periph == NULL);

	return indigo_generic_reset(periph);
}

int gsm_sim900D_setup(struct gpio_peripheral *periph)
{
	int result;

	TRACE_ENTRY();

	BUG_ON(periph == NULL);

	result = gsm_generic_simcom_setup(periph, keep_turned_on_handler_irq);

	periph->status = gsm_generic_status;
	periph->power_on = gsm_sim900D_power_on;
	periph->power_off = gsm_sim900D_power_off;
	periph->reset = gsm_sim900D_reset;
	periph->check_and_power_on = indigo_check_and_power_on;

	indigo_peripheral_create_command(periph, INDIGO_COMMAND_CHECK_AND_POWER_ON);

	TRACE_EXIT_RES(result);
	return result;
}
EXPORT_SYMBOL(gsm_sim900D_setup);

/*
 * ------------------------------------------------------
 * ------------------------------------------------------
 */

/*
 * SimCOM Sim900 GSM Module (Device 1.1 and after)
 *
 * @dev -- struct gpio_peripheral, describing the modem
 *
 * All operations are as per Sim900D Hardware Design v...
 */

/* figure 9, pg 25 */
int gsm_sim900_power_on(const struct gpio_peripheral *periph)
{
	int status = 0;

	int timeout = 0;

	struct indigo_gpio_sequence_step steps[] = {

		{"0", "turn on POWER pin if available",
		 periph, INDIGO_FUNCTION_POWER, 0, false, 0, 0},

		{"1", "pwrkey to 1 for 0.5s -- nonstrict, ends at t0",
		 periph, INDIGO_FUNCTION_PWRKEY, 1, true, 500, 0},

		{"2", "pwrkey to 0 for t - t0 > 1s -- strict",
		 periph, INDIGO_FUNCTION_PWRKEY, 0, true, 1100, 0},

		{"3", "pwrkey to 1",
		 periph, INDIGO_FUNCTION_PWRKEY, 1, true, 0, 0},

		/* monitor status pin for value 1 */
		{"4", "wait for status pin to come up for more than t - t0 > 2.2 s",
		 periph, INDIGO_FUNCTION_STATUS, 1, true, 0, 10000},

		{"5", "finally, status pin is 1 when all is ok",
		 NULL, INDIGO_FUNCTION_NO_FUNCTION, 0, true, 0, 0}
	};


	TRACE_ENTRY();

	BUG_ON(periph == NULL);

	/* initially, status pin is 0 -- modem is turned off */
	if (periph->status(periph)) {
		printk(KERN_ERR "tried to power on device with status pin 1");
		BUG();
		return -ENODEV;
	}

	indigo_gpio_perform_sequence((struct indigo_gpio_sequence_step **) &steps, ARRAY_SIZE(steps));

	status = periph->status(periph);
	PRINT(KERN_ERR, "status pin is %d", status);

	TRACE_EXIT_RES(!status);
	return !status; /* 0 is OK code, 1 -- error */
}

int gsm_sim900_power_off(const struct gpio_peripheral *periph)
{
	int status;
	int timeout = 0;

	struct indigo_gpio_sequence_step steps[] = {

		{"1", "pwrkey -> 0 for 5s < t < 1s",
		 periph, INDIGO_FUNCTION_PWRKEY, 0, true, 2000, 0},

		{"2", "set PWRKEY -> 1",
		 periph, INDIGO_FUNCTION_PWRKEY, 1, true, 50, 0},

		/* monitor status pin for value 0 */
		{"3", "wait for > 1.7 seconds",
		 periph, INDIGO_FUNCTION_STATUS, 0, true, 0, 10000},

		{"4", "finally, status pin is 0 when all is ok",
		 NULL, INDIGO_FUNCTION_NO_FUNCTION, 0, true, 0, 0}
	};

	TRACE_ENTRY();

	/* same as sim900 */

	BUG_ON(periph == NULL);

	/* initially, status pin is 1 -- modem is turned on */
	if (!periph->status(periph)) {
		printk(KERN_ERR "tried to power off device with status pin 0");
		BUG();
		return -ENODEV;
	}

	indigo_gpio_perform_sequence((struct indigo_gpio_sequence_step **) &steps, ARRAY_SIZE(steps));

	status = periph->status(periph);
	PRINT(KERN_ERR, "status pin is %d", status);

	TRACE_EXIT_RES(status);
	return !status; /* 0 is OK code, 1 -- is error */

}

int gsm_sim900_reset(const struct gpio_peripheral *periph)
{
	return indigo_generic_reset(periph);
}

int gsm_sim900_setup(struct gpio_peripheral *periph)
{
	int result;
	TRACE_ENTRY();

	result = gsm_generic_simcom_setup(periph, keep_turned_on_handler_irq);
	if (result) {
		PRINT(KERN_ERR, "error in generic_simcom_setup");
		goto out;
	}

	periph->reset = gsm_sim900_reset;
	periph->status = gsm_generic_status;
	periph->power_on = gsm_sim900_power_on;
	periph->power_off = gsm_sim900_power_off;
	periph->check_and_power_on = indigo_check_and_power_on;

	indigo_peripheral_create_command(periph, INDIGO_COMMAND_CHECK_AND_POWER_ON);
out:
	TRACE_EXIT_RES(result);
	return result;
}
EXPORT_SYMBOL(gsm_sim900_setup);

/*
 * ------------------------------------------------------
 * ------------------------------------------------------
 */

/*
 * SimCOM Sim508 GPS Module (Starterkit devices)
 *
 * Manual used is Sim508 Hardware Design 2.08
 */
static int gps_sim508_status(const struct gpio_peripheral *periph)
{
	int power_pin;
	int result;

	TRACE_ENTRY();

	BUG_ON(periph == NULL);

	power_pin = indigo_gpioperiph_get_pin_by_function(periph, INDIGO_FUNCTION_POWER);

	result = indigo_pin_active_value(&periph->pins[power_pin],
		gpio_get_value(periph->pins[power_pin].pin_no));

	TRACE_EXIT_RES(result);
	return result;
}

/* figure 28 */
int gps_sim508_power_on(const struct gpio_peripheral *periph)
{
	int status;
	struct indigo_gpio_sequence_step steps[] = {
		{"1", "set power to on and wait 220 ms",
		 periph, INDIGO_FUNCTION_POWER, 1, true, 220, 0},

	};

	TRACE_ENTRY();

	BUG_ON(periph == NULL);

	if (periph->status(periph)) {
		printk(KERN_ERR "GPS already seems to work\n");
		BUG();
		return -ENODEV;
	}

	indigo_gpio_perform_sequence((struct indigo_gpio_sequence_step **) &steps, ARRAY_SIZE(steps));

	status = periph->status(periph);
	PRINT(KERN_ERR, "status pin is %d", status);

	TRACE_EXIT_RES(status);
	return !status;
}

/* no precise way to nicely turn this off */
int gps_sim508_power_off(const struct gpio_peripheral *periph)
{
	TRACE_ENTRY();

	BUG_ON(periph == NULL);

	if (!periph->status(periph)) {
		printk(KERN_ERR "GPS already seems to be turned off\n");
		BUG();
		return -ENODEV;
	}

	/* step 1. set power pin to 0 */
	indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_POWER, 0, true);

	TRACE_EXIT();
	return 0;
}

int gps_sim508_reset(const struct gpio_peripheral *periph)
{
	int result;

	TRACE_ENTRY();

	BUG_ON(periph == NULL);

	result = periph->reset(periph);

	TRACE_EXIT_RES(result);
	return result;
}

int gps_sim508_setup(struct gpio_peripheral *periph)
{
	TRACE_ENTRY();

	BUG_ON(periph == NULL);

	indigo_configure_pin(periph, INDIGO_FUNCTION_POWER, /* mandatory */ true);

	indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_POWER, 1, true);

	periph->power_on = gps_sim508_power_on;
	periph->power_off = gps_sim508_power_off;
	periph->status = gps_sim508_status;
	periph->reset = gps_sim508_reset;
	periph->check_and_power_on = indigo_check_and_power_on;

	TRACE_EXIT();
	return 0;
}
EXPORT_SYMBOL(gps_sim508_setup);
/*
 * ----------------------------------------------------------
 * ----------------------------------------------------------
 */

/*
 * EB-500 GPS Module (Device 1.0)
 *
 */
int gps_eb500_power_on(const struct gpio_peripheral *periph)
{
	int result;

	TRACE_ENTRY();

	BUG_ON(periph == NULL);


	TRACE_EXIT_RES(result);
	return result;
}

int gps_eb500_power_off(const struct gpio_peripheral *periph)
{
	int result;

	TRACE_ENTRY();

	BUG_ON(periph == NULL);

	indigo_generic_reset(periph);

	TRACE_EXIT_RES(result);
	return result;
}

int gps_eb500_reset(const struct gpio_peripheral *periph)
{
	int result;

	TRACE_ENTRY();

	BUG_ON(periph == NULL);


	TRACE_EXIT_RES(result);
	return result;
}

int gps_eb500_setup(struct gpio_peripheral *periph)
{
	int result;

	TRACE_ENTRY();

	BUG_ON(periph == NULL);


	TRACE_EXIT_RES(result);
	return result;
}
EXPORT_SYMBOL(gps_eb500_setup);

/*
 * NV80C-CSM GPS/GNSS Module (Hardware V2.1)
 *
 */

int gps_nv08c_csm_power_on(const struct gpio_peripheral *periph)
{
	int result;

	TRACE_ENTRY();

	BUG_ON(periph == NULL);


	TRACE_EXIT_RES(result);
	return result;
}

int gps_nv08c_csm_power_off(const struct gpio_peripheral *periph)
{
	int result;

	TRACE_ENTRY();

	BUG_ON(periph == NULL);


	TRACE_EXIT_RES(result);
	return result;
}

int gps_nv08c_csm_reset(const struct gpio_peripheral *periph)
{
	int result;

	TRACE_ENTRY();

	BUG_ON(periph == NULL);


	TRACE_EXIT_RES(result);
	return result;
}

int gps_nv08c_csm_setup(struct gpio_peripheral *periph)
{
	int result;

	TRACE_ENTRY();

	BUG_ON(periph == NULL);

	periph->reset = gps_nv08c_csm_reset;
	periph->power_on = gps_nv08c_csm_power_on;
	periph->power_off = gps_nv08c_csm_power_off;
	periph->check_and_power_on = indigo_check_and_power_on;


	TRACE_EXIT_RES(result);
	return result;
}
EXPORT_SYMBOL(gps_nv08c_csm_setup);

/* sysfs interface to previous code */

/*
 * The default show function that must be passed to sysfs.  This will be
 * called by sysfs for whenever a show function is called by the user on a
 * sysfs file associated with the kobjects we have registered.  We need to
 * transpose back from a "default" kobject to our custom struct foo_obj and
 * then call the show function for that specific object.
 */
static ssize_t gpio_peripheral_attr_show(struct kobject *kobj,
                             struct attribute *attr,
                             char *buf)
{
        struct gpio_peripheral_attribute *attribute;
        struct gpio_peripheral_obj *peripheral_obj;
	int ret = -EIO;

        attribute = to_gpio_peripheral_attr(attr);
        peripheral_obj = to_gpio_peripheral_obj(kobj);

        if (attribute->show)
		ret = attribute->show(peripheral_obj, attribute, buf);

	return ret;
}

/*
 * Just like the default show function above, but this one is for when the
 * sysfs "store" is requested (when a value is written to a file.)
 */
static ssize_t gpio_peripheral_attr_store(struct kobject *kobj,
                              struct attribute *attr,
                              const char *buf, size_t len)
{
        struct gpio_peripheral_attribute *attribute;
        struct gpio_peripheral_obj *peripheral_obj;
	int ret = -EIO;

        attribute = to_gpio_peripheral_attr(attr);
        peripheral_obj = to_gpio_peripheral_obj(kobj);

        if (!attribute->store)
                ret = attribute->store(peripheral_obj, attribute, buf, len);

        return ret;
}

/*
 * The release function for our object.  This is REQUIRED by the kernel to
 * have.  We free the memory held in our object here.
 *
 * NEVER try to get away with just a "blank" release function to try to be
 * smarter than the kernel.  Turns out, no one ever is...
 */
static void indigo_gpio_peripheral_obj_release(struct kobject *kobj)
{
	struct gpio_peripheral_obj *peripheral_obj;

	TRACE_ENTRY();

	peripheral_obj = to_gpio_peripheral_obj(kobj);

	flush_workqueue(peripheral_obj->wq);
	destroy_workqueue(peripheral_obj->wq);

	kfree(peripheral_obj);

	TRACE_EXIT();
}

/*
 * выполнить команду из work_struct в контексте workqueue
 */
static void indigo_peripheral_process_command(struct work_struct *command)
{
	struct gpio_peripheral_command *gp_cmd;
	struct gpio_peripheral *peripheral;
	struct gpio_peripheral_obj *peripheral_obj;

	unsigned long flags;

	TRACE_ENTRY();

	/* execute command */

	gp_cmd = container_of(command, struct gpio_peripheral_command, work);
	peripheral = gp_cmd->peripheral;

	BUG_ON(peripheral == NULL);

	peripheral_obj = container_of(peripheral, struct gpio_peripheral_obj, peripheral);
	BUG_ON(peripheral_obj == NULL); /* на самом деле невозможно */

	switch (gp_cmd->cmd) {
	case INDIGO_COMMAND_NO_COMMAND:
		printk(KERN_INFO "NO_COMMAND is issued\n");
		break;
	case INDIGO_COMMAND_POWER_ON:
		peripheral->power_on(peripheral);
		break;
	case INDIGO_COMMAND_POWER_OFF:
		peripheral->reset(peripheral);
		break;
	case INDIGO_COMMAND_RESET:
		peripheral->reset(peripheral);
		break;
	case INDIGO_COMMAND_CHECK_AND_POWER_ON:
		peripheral->check_and_power_on(peripheral);
		break;
	default:
		printk(KERN_ERR "unknown command supplied\n");
	}

	/* remove executed command from list */
	spin_lock_irqsave(&peripheral_obj->command_list_lock, flags);
	/* i assume it's safe do free work_struct here, because: */
	/*
	 * f(work);

	 * While we must be careful to not use "work" after this, the trace
	 * point will only record its address.
	 *
	 * trace_workqueue_execute_end(work);
	 */
	list_del(&gp_cmd->command_sequence);
	kfree(command);

	if (list_empty(&peripheral_obj->command_list)) {
		complete(&peripheral_obj->command_list_empty);
		INIT_COMPLETION(peripheral_obj->command_list_empty);
	}
	spin_unlock_irqrestore(&peripheral_obj->command_list_lock, flags);

	TRACE_EXIT();
}

/* создать, поместить в очередь
 *
 * CONTEXT: atomic or process
 */
void indigo_peripheral_create_command(struct gpio_peripheral *peripheral,
	enum indigo_gpioperiph_command_t command)
{
	struct gpio_peripheral_command *gp_cmd;
	struct gpio_peripheral_obj *peripheral_obj;

	TRACE_ENTRY();

	BUG_ON(peripheral == NULL);
	peripheral_obj = container_of(peripheral, struct gpio_peripheral_obj, peripheral);

	/* FIXME SLUB */
	gp_cmd = kzalloc(sizeof(*gp_cmd), in_atomic() ? GFP_ATOMIC : GFP_KERNEL);
	if (!gp_cmd) {
		printk(KERN_ERR "no memory for gp_cmd\n");
		goto out;
	}

	gp_cmd->cmd = command;
	gp_cmd->peripheral = peripheral;

	INIT_WORK(&gp_cmd->work, indigo_peripheral_process_command);

	INIT_LIST_HEAD(&gp_cmd->command_sequence);
	list_add_tail(&gp_cmd->command_sequence, &peripheral_obj->command_list);

	queue_work(peripheral_obj->wq, &gp_cmd->work);

out:
	TRACE_EXIT();
	return;
}

/* here interfaces go */

/**
 * @attr seem to be used for several different attributes,
 * which is not our case. At least for now
 */
static ssize_t status_show(struct gpio_peripheral_obj *peripheral_obj, struct gpio_peripheral_attribute *attr,
                        char *buf)
{
	struct gpio_peripheral *periph;
	ssize_t len;

	TRACE_ENTRY();

	BUG_ON(peripheral_obj == NULL);

	periph = &peripheral_obj->peripheral;

	BUG_ON(periph == NULL);

	len = sprintf(buf, "%s\n", periph->status(periph) == 1 ? "on" : "off");

	TRACE_EXIT();
        return len;
}

static ssize_t dummy_store(struct gpio_peripheral_obj *periph_obj, struct gpio_peripheral_attribute *attr,
                         const char *buf, size_t count)
{
        return count; /* do nothing at all */
}

static ssize_t dummy_show(struct gpio_peripheral_obj *periph_obj, struct gpio_peripheral_attribute *attr,
                         char *buf)
{
        return 0; /* do nothing at all */
}

/*
ssize_t (*show)(struct gpio_peripheral_obj *peripheral_obj,
		struct gpio_peripheral_attribute *attr,
		char *buf, size_t count);
*/
/* доступ к GPIO на чтение */
ssize_t gpio_show(struct gpio_peripheral_obj *peripheral_obj,
		  struct gpio_peripheral_attribute *attr,
		  char *buf)
{
	int len;
	struct indigo_periph_pin *pin;

	TRACE_ENTRY();

	pin = container_of(attr, struct indigo_periph_pin, sysfs_attr);
	len = sprintf(buf, "%d\n", gpio_get_value(pin->pin_no));

	TRACE_EXIT();
        return len;
}

static ssize_t power_on_store(struct gpio_peripheral_obj *peripheral_obj, struct gpio_peripheral_attribute *attr,
                         const char *buf, size_t count)
{
	TRACE_ENTRY();

	BUG_ON(peripheral_obj == NULL);

	indigo_peripheral_create_command(&peripheral_obj->peripheral, INDIGO_COMMAND_POWER_ON);
	wait_for_completion_interruptible(&peripheral_obj->command_list_empty);

	TRACE_EXIT();
        return count;
}

static ssize_t check_and_power_on_store(struct gpio_peripheral_obj *peripheral_obj, struct gpio_peripheral_attribute *attr,
                         const char *buf, size_t count)
{
	TRACE_ENTRY();

	BUG_ON(peripheral_obj == NULL);

	indigo_peripheral_create_command(&peripheral_obj->peripheral, INDIGO_COMMAND_CHECK_AND_POWER_ON);
	wait_for_completion_interruptible(&peripheral_obj->command_list_empty);

	TRACE_EXIT();
        return count;
}

static ssize_t power_off_store(struct gpio_peripheral_obj *peripheral_obj, struct gpio_peripheral_attribute *attr,
                         const char *buf, size_t count)
{
	TRACE_ENTRY();

	BUG_ON(peripheral_obj == NULL);

	indigo_peripheral_create_command(&peripheral_obj->peripheral, INDIGO_COMMAND_POWER_OFF);
	wait_for_completion_interruptible(&peripheral_obj->command_list_empty);

	TRACE_EXIT();
        return count;
}

static ssize_t reset_store(struct gpio_peripheral_obj *peripheral_obj, struct gpio_peripheral_attribute *attr,
                         const char *buf, size_t count)
{
	TRACE_ENTRY();

	BUG_ON(peripheral_obj == NULL);

	indigo_peripheral_create_command(&peripheral_obj->peripheral, INDIGO_COMMAND_RESET);
	wait_for_completion_interruptible(&peripheral_obj->command_list_empty);

	TRACE_EXIT();
        return count;
}

/* Our custom sysfs_ops that we will associate with our ktype later on */
static const struct sysfs_ops gpio_peripheral_sysfs_ops = {
        .show = gpio_peripheral_attr_show,
        .store = gpio_peripheral_attr_store,
};

/* здесь, похоже, нужна будет динамическая инициализация, ничего страшного */
static struct gpio_peripheral_attribute gpio_peripheral_attributes_default[] =
{
	__ATTR(power_on, 0666, dummy_show, power_on_store),
	__ATTR(power_off, 0666, dummy_show, power_off_store),
	__ATTR(reset, 0666, dummy_show, reset_store),
	__ATTR(status, 0666, status_show, dummy_store),
	__ATTR(check_and_power_on, 0666, dummy_show, check_and_power_on_store)
};

/*
 * Create a group of attributes so that we can create and destroy them all
 * at once.
 */
static struct attribute *gpio_peripheral_attributes_default_sysfs[] = {
	&gpio_peripheral_attributes_default[0].attr,
	&gpio_peripheral_attributes_default[1].attr,
	&gpio_peripheral_attributes_default[2].attr,
	&gpio_peripheral_attributes_default[3].attr,
	&gpio_peripheral_attributes_default[4].attr,
        NULL,   /* need to NULL terminate the list of attributes */
};

/*
 * Our own ktype for our kobjects.  Here we specify our sysfs ops, the
 * release function, and the set of default attributes we want created
 * whenever a kobject of this type is registered with the kernel.
 */
static struct kobj_type gpio_peripheral_ktype = {
        .sysfs_ops = &gpio_peripheral_sysfs_ops,
        .release = indigo_gpio_peripheral_obj_release,
        .default_attrs = gpio_peripheral_attributes_default_sysfs
};

static struct kset *indigo_kset;
static LIST_HEAD(kobjects);

struct gpio_peripheral_obj *create_gpio_peripheral_obj(struct gpio_peripheral *peripheral)
{
        struct gpio_peripheral_obj *peripheral_obj;
        int retval;
	int i;

        /* allocate the memory for the whole object */
        peripheral_obj = kzalloc(sizeof(*peripheral_obj), GFP_KERNEL);
        if (!peripheral_obj)
                goto out;

        /*
         * As we have a kset for this kobject, we need to set it before calling
         * the kobject core.
         */
        peripheral_obj->kobj.kset = indigo_kset;

	/* copy our static structure to kmalloc memory */
	peripheral_obj->peripheral = *peripheral;

	init_completion(&peripheral_obj->command_list_empty);
	spin_lock_init(&peripheral_obj->command_list_lock);
	INIT_LIST_HEAD(&peripheral_obj->command_list);
	/* implies that now peripheral->name should be unique */
	peripheral_obj->wq = alloc_ordered_workqueue(peripheral->name, 0);

        /*
         * Initialize and add the kobject to the kernel.  All the default files
         * will be created here.  As we have already specified a kset for this
         * kobject, we don't have to set a parent for the kobject, the kobject
         * will be placed beneath that kset automatically.
         */
        retval = kobject_init_and_add(&peripheral_obj->kobj,
		&gpio_peripheral_ktype, NULL, "%s", peripheral->name);
        if (retval)
		goto out_put;

	/* --------------------------------------- */
	indigo_configure_general_pins(peripheral);
	/* --------------------------------------- */

	for (i = 0; i < INDIGO_MAX_GPIOPERIPH_PIN_COUNT; i++) {
		if (peripheral->pins[i].description == NULL)
			break;

		/* read-only attribute */
		peripheral->pins[i].sysfs_attr.attr.name = peripheral->pins[i].schematics_name;
		peripheral->pins[i].sysfs_attr.attr.mode = 0444;
		peripheral->pins[i].sysfs_attr.show = gpio_show;
		peripheral->pins[i].sysfs_attr.store = dummy_store;
		/* add file to sysfs. not too sure about rolling back */
		sysfs_create_file(&peripheral_obj->kobj, &peripheral->pins[i].sysfs_attr.attr);
	}

	/* in order to release all of 'em */
	INIT_LIST_HEAD(&peripheral_obj->kobject_item);
	list_add_tail(&peripheral_obj->kobject_item, &kobjects);

	/* ------------------------------------------ */
	peripheral_obj->peripheral.setup(&peripheral_obj->peripheral);
	/* ------------------------------------------ */

        /*
         * We are always responsible for sending the uevent that the kobject
         * was added to the system.
         */
        kobject_uevent(&peripheral_obj->kobj, KOBJ_ADD);

out:
        return peripheral_obj;

out_put:
	kobject_put(&peripheral_obj->kobj);

	peripheral_obj = NULL;
	goto out;
}

static void destroy_gpio_peripheral_obj(struct gpio_peripheral_obj *gpio_peripheral_obj)
{
        kobject_put(&gpio_peripheral_obj->kobj);
}

/* единственный ужас -- 3 устройства */
static struct gpio_peripheral indigo_gpioperiph_platform_data[3];

static struct platform_device indigo_gpioperiph_device = {
        .name           = "indigo_gpioperiph",
        .id             = -1,
        .dev            = {
		.platform_data = &indigo_gpioperiph_platform_data,
        }
};


int __init indigo_gpio_peripheral_init(struct gpio_peripheral peripherals[3], int nr_devices)
{
	int result;
	int i;
	struct gpio_peripheral_obj *periph_obj;

        /*
         * Create a kset with the name of "kset_example",
         * located under /sys/kernel/
         */
        indigo_kset = kset_create_and_add("indigo", NULL, kernel_kobj);
        if (!indigo_kset)
                result = -EIO;

	memcpy(&indigo_gpioperiph_platform_data, peripherals, sizeof(peripherals[0]) * 3);

	for (i = 0; i < nr_devices; i++) {
		periph_obj = create_gpio_peripheral_obj(&peripherals[i]);
		if (!periph_obj) {
			printk(KERN_ERR "fatal error during object creation\n");
			goto out;
		}

		printk(KERN_ERR "indigo gpioperiph: %s peripheral %s added\n",
		       periph_obj->peripheral.name, periph_obj->peripheral.description);
	}

	platform_device_register(&indigo_gpioperiph_device);

out:
        return result;

}

void __exit indigo_gpio_peripheral_exit(void)
{
	struct gpio_peripheral_obj *obj, *tmp;
	/* we need to correctly destroy all objects here, not sure about attributes */
	list_for_each_entry_safe(obj, tmp, &kobjects, kobject_item) {
		destroy_gpio_peripheral_obj(obj);
		list_del(&obj->kobject_item);
	}
        kset_unregister(indigo_kset);
}

EXPORT_SYMBOL(indigo_gpio_peripheral_exit);
EXPORT_SYMBOL(indigo_gpio_peripheral_init);
EXPORT_SYMBOL(create_gpio_peripheral_obj);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yury Luneff <yury@indigosystem.ru>");
MODULE_DESCRIPTION("Generic GPIO peripheral driver with some predefined peripherals");
