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

#include <linux/indigo-gpioperiph.h>

static uint8_t do_debug_output = 0;

module_param_named(debug, do_debug_output, byte, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Do debug output");

#define PRINT(log_flag, format, args...)		\
	if (unlikely(do_debug_output))			\
		printk(log_flag format "\n", ## args)

#define TRACE_ENTRY()					\
	if (unlikely(do_debug_output))			\
                PRINT(KERN_INFO, "ENTRY %s", __func__);

#define TRACE_EXIT()					\
	if (unlikely(do_debug_output))			\
                PRINT(KERN_INFO, "LEAVE %s", __func__);

#define TRACE_EXIT_RES(res)						\
	if (unlikely(do_debug_output))					\
                PRINT(KERN_INFO, "LEAVE %s result %d", __func__, res);

#define TRACE_STEP(step_name, message)					\
	if (unlikely(do_debug_output))					\
		PRINT(KERN_INFO, "step %s message %s", step_name, message);

#define sBUG() do {                                             \
        printk(KERN_CRIT "BUG at %s:%d\n",                      \
               __FILE__, __LINE__);                             \
        local_irq_enable();                                     \
        while (in_softirq())                                    \
                local_bh_enable();                              \
        BUG();                                                  \
} while (0)

#define sBUG_ON(p) do {                                         \
        if (unlikely(p)) {                                      \
                printk(KERN_CRIT "BUG at %s:%d (%s)\n",         \
                       __FILE__, __LINE__, #p);                 \
                local_irq_enable();                             \
                while (in_softirq())                            \
                        local_bh_enable();                      \
                BUG();                                          \
        }                                                       \
} while (0)

static struct kmem_cache *indigo_cmd_mem_cache = NULL;

int indigo_gpioperiph_get_pin_by_function(struct gpio_peripheral *periph,
					enum indigo_pin_function_t function)
{
	int i;
	int pin_found = INDIGO_NO_PIN;

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	for (i = 0; i < INDIGO_MAX_GPIOPERIPH_PIN_COUNT; i++) {
		if (periph->pins[i].function == function) {
			pin_found = i;
			break;
		}

		if (periph->pins[i].description == NULL)
			break;
	}

	TRACE_EXIT_RES(pin_found);
	/* maybe this pin is not that crucial */
	return pin_found;
}

/* kernel panic if pin's not found, thus no error handling need in caller */
int indigo_gpioperiph_get_mandatory_pin_by_function(struct gpio_peripheral *periph,
						enum indigo_pin_function_t function,
						bool mandatory)
{
	int pin = 0;

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	pin = indigo_gpioperiph_get_pin_by_function(periph, function);

	if (mandatory && pin == INDIGO_NO_PIN) {
		sBUG();
		panic("couldn't find function %d for periph %s\n",
			function, periph->description);
	}

	TRACE_EXIT();

	return pin;
}

/* нужно просто <s>хорошо работать</s> сказать sysfs_notify на нужный объект */
static irqreturn_t indigo_pin_notify_change_handler(int irq, void *priv)
{
	struct work_struct *work = priv;


	(void) irq;
	/* printk(KERN_ERR "I'm here! %s\n", pin->schematics_name); */
	schedule_work(work);

	return IRQ_HANDLED;
}

static void indigo_pin_notify_sysfs(struct work_struct *work)
{
	struct indigo_periph_pin *pin = container_of(work, struct indigo_periph_pin, work);

	if (pin->value_sd != NULL)
		sysfs_notify_dirent(pin->value_sd);
}

/* смысл, в основном, в том, чтобы дополнить разницу
 * между общими функциями gpio_* и атмеловские at91_*
 */
int indigo_request_pin(const struct indigo_periph_pin *pin)
{
	int result;

	TRACE_ENTRY();

	sBUG_ON(pin == NULL);
	result = gpio_request(pin->pin_no, pin->schematics_name);
	if (result) {
		printk(KERN_ERR "failed to request pin %s #%d with result %d\n", pin->schematics_name, pin->pin_no, result);
		sBUG();
		panic("failed to request pin %s\n", pin->schematics_name);
	}

	PRINT(KERN_INFO, "pin number %d, current value get %d", pin->pin_no, gpio_get_value(pin->pin_no));
	if (pin->flags & GPIOF_INIT_HIGH) {
		PRINT(KERN_INFO, "GPIOF_INIT_HIGH,");
	} else {
		PRINT(KERN_INFO, "GPIOF_INIT_LOW,");
	}

	if (pin->flags & GPIOF_DIR_IN) {
		PRINT(KERN_INFO, "GPIOF_DIR_IN,");
	} else {
		PRINT(KERN_INFO, "GPIOF_DIR_OUT,");
	}

	if (pin->flags & GPIOF_PULLUP) {
		PRINT(KERN_INFO, "GPIOF_PULLUP");
	} else {
		PRINT(KERN_INFO, "GPIOF_NO_PULLUP");
	}

	if ((pin->flags & GPIOF_DIR_IN) != 0) {
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

	TRACE_EXIT();
	return result;
}

/* get_by_function + request */
static int indigo_configure_pin(
	struct gpio_peripheral *periph,
	enum indigo_pin_function_t function,
	bool mandatory)
{
	int pin;

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	pin = indigo_gpioperiph_get_mandatory_pin_by_function(periph,
							function, mandatory);
	if ((pin == INDIGO_NO_PIN) && !mandatory)
		goto done;

	indigo_request_pin(&periph->pins[pin]);

done:
	TRACE_EXIT_RES(pin == INDIGO_NO_PIN);
	return pin;
}

/*
 * Invert @value arg if GPIOF_ACTIVE_LOW flag is set for this pin
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
	sBUG_ON(pin == NULL);

	return ((pin->flags & GPIOF_ACTIVE_LOW) != 0) ^ value;
}

int indigo_configure_general_pins(struct gpio_peripheral *periph)
{
	int i;
	int result = 0;

	TRACE_ENTRY();
	sBUG_ON(periph == NULL);

	for (i = 0; i < INDIGO_MAX_GPIOPERIPH_PIN_COUNT; i++) {
		/* маркер конца списка */
		if (periph->pins[i].description == NULL)
			break;

		/* остальные пины ушли в инициализации девайсов */
		if (periph->pins[i].function != INDIGO_FUNCTION_NO_FUNCTION)
			continue;

		result = indigo_request_pin(&periph->pins[i]);
		if (result)
			break;
	}

	TRACE_EXIT_RES(result);
	return result;
}
EXPORT_SYMBOL(indigo_configure_general_pins);

/**
 * only valid for output pins
 *
 * @value is corrected by @function pin flags
 */
static void indigo_gpioperiph_set_output(struct gpio_peripheral *periph,
					enum indigo_pin_function_t function,
					int value,
					bool mandatory)
{
	int pin;

	pin = indigo_gpioperiph_get_mandatory_pin_by_function(periph, function, mandatory);

	if (pin == INDIGO_NO_PIN) {
		PRINT(KERN_INFO, "non-mandatory pin for function %d not found\n", function);
		goto done;
	}

	/* otherwise, there'll be panic */

	if ((periph->pins[pin].flags & GPIOF_DIR_IN) != 0) {
		printk(KERN_ERR "tried to output to input pin %d\n", pin);
		sBUG();
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
 * context: !in_atomic()
 *
 * @INDIGO_FUNCTION_STATUS as pin kind is handled by timeout
 */
static int indigo_gpio_perform_sequence(struct indigo_gpio_sequence_step *steps,
					int step_count)
{
	int i;
	int result = 0;
	int status;
	int timeout;

	for (i = 0; i < step_count; i++) {
		TRACE_STEP(steps[i].step_no, steps[i].step_desc);

		/* last step indicator */
		if (steps[i].periph == NULL)
			goto out;

		/* function is not mandatory when it's just a timeout waiting */
		if (steps[i].function != INDIGO_FUNCTION_NO_FUNCTION &&
			steps[i].function != INDIGO_FUNCTION_STATUS) {

			indigo_gpioperiph_set_output(steps[i].periph,
						steps[i].function, steps[i].value, steps[i].mandatory);
		}

		if (steps[i].sleep_ms != 0)
			msleep(steps[i].sleep_ms);

		timeout = 0;
		/* only timeout on status function available */
		if (steps[i].timeout_ms != 0 && steps[i].function == INDIGO_FUNCTION_STATUS) {
			status = steps[i].periph->status(steps[i].periph);
			/* wait for given status value if INDIGO_FUNCTION_STATUS happened*/
			while (timeout < steps[i].timeout_ms && (status != steps[i].value)) {
				msleep(500);
				timeout = timeout + 500;
				status = steps[i].periph->status(steps[i].periph);
			}
			result = !status;
		}
	}

out:
	TRACE_EXIT_RES(result);
	return result;
}

/* general GSM routines */

/**
 * Returns status pin value w/o interpretation
 */
static int gsm_generic_status(struct gpio_peripheral *periph)
{
	int status_pin = indigo_gpioperiph_get_pin_by_function(periph,
							INDIGO_FUNCTION_STATUS);

	return indigo_pin_active_value(
		&periph->pins[status_pin],
		gpio_get_value(periph->pins[status_pin].pin_no));
}

static struct completion *indigo_peripheral_create_command(struct gpio_peripheral *peripheral,
							enum indigo_gpioperiph_command_t command);

static struct completion *indigo_peripheral_create_command_arg(struct gpio_peripheral *peripheral,
							enum indigo_gpioperiph_command_t command, int argument);


static irqreturn_t keep_turned_on_handler_irq(int irq, void *dev)
{
	struct gpio_peripheral *device = (struct gpio_peripheral *) dev;
	struct gpio_peripheral_obj *obj;

	(void)irq;

	TRACE_ENTRY();

	obj = container_of(device, struct gpio_peripheral_obj, peripheral);

	schedule_work(&obj->check_status_work);
	/*
	 * schedule a check of device status at the end of command queue
	 * and turn it on if status is 0 that time
	 */

	TRACE_EXIT();

	return IRQ_HANDLED;
}

static void indigo_check_status(struct work_struct *work)
{
	struct gpio_peripheral_obj *peripheral_obj = NULL;
	struct gpio_peripheral *device = NULL;
	TRACE_ENTRY();

	peripheral_obj = container_of(work, struct gpio_peripheral_obj, check_status_work);
	device = &peripheral_obj->peripheral;

	PRINT(KERN_INFO, "status reading is %d\n", device->status(device));
	if (!device->status(device))
		indigo_peripheral_create_command(device, INDIGO_COMMAND_CHECK_AND_POWER_ON);

	TRACE_EXIT();
	return;
}

/* шлём либо NULL, либо keen_turned_on_handler_irq */
static int indigo_set_keep_on_handler(struct gpio_peripheral *periph,
				irq_handler_t status_pin_handler)
{
	int status;
	int result = 0;

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	status = indigo_gpioperiph_get_pin_by_function(periph, INDIGO_FUNCTION_STATUS);
	if (status == INDIGO_NO_PIN) {
		result = -ENOENT;
		goto out;
	}

	if ((periph->current_state != GPIO_PERIPH_STATE_GSM_KEEP_ON) && (status_pin_handler != NULL)) {
		if (request_irq(gpio_to_irq(periph->pins[status].pin_no),
					status_pin_handler,
					IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
					periph->pins[status].description, (void *) periph)) {

			printk(KERN_ERR "can not request irq for status pin\n");
			panic("can not request irq for status pin\n");
		}
		periph->current_state = GPIO_PERIPH_STATE_GSM_KEEP_ON;
	} else if (status_pin_handler == NULL) {
		/* если не проверять состояние, можем влететь в двойной free_irq */
		free_irq(gpio_to_irq(periph->pins[status].pin_no), (void *) periph);

		/* FIXME не уверен, что это здесь необходимо */
		switch (periph->status(periph)) {
		case 0:
			periph->current_state = GPIO_PERIPH_STATE_GSM_OFF;
			break;
		case 1:
			periph->current_state = GPIO_PERIPH_STATE_GSM_ON;
		}
	} else {
		printk(KERN_ERR "don't try to get to the same state twice\n");
		result = -EINVAL;
	}

out:
	TRACE_EXIT_RES(result);
	return result;
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
	int result = 0;
	int status;

	TRACE_ENTRY();

	/* we will not return from there if configuration has failed */

	status = indigo_configure_pin(periph, INDIGO_FUNCTION_STATUS, /* mandatory */ true);

	periph->status = gsm_generic_status;

	if (status_pin_handler != NULL)
		indigo_set_keep_on_handler(periph, status_pin_handler);

	indigo_configure_pin(periph, INDIGO_FUNCTION_PWRKEY, /* mandatory */ true);

	/* it doesn't really matter if it's not found */
	indigo_configure_pin(periph, INDIGO_FUNCTION_POWER, /* mandatory */ false);

	TRACE_EXIT_RES(result);
	return result;
}

/* FIXME error handling through int result */
static int indigo_generic_reset(struct gpio_peripheral *periph)
{
	int result = 0;

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	TRACE_STEP("1", "restart");
	if (periph->status(periph)) {
		/* option 1. restart */
		TRACE_STEP("1.1", "Power off");

		result = periph->power_off(periph);

		if (result) {
			PRINT(KERN_ERR, "couldn't power off the device");
			goto out;
		}
	}

	TRACE_STEP("1.2", "Power on");
	result = periph->power_on(periph);
	if (result) {
		PRINT(KERN_ERR, "couldn't power on the device");
		goto out;
	}

out:
	TRACE_EXIT_RES(result);
	return result;
}

int indigo_gpio_do_nothing(struct gpio_peripheral *periph)
{
	TRACE_ENTRY();

	(void) periph;

	TRACE_EXIT_RES(0);
	return 0;
}

/* заглушка для всяких BQ */
int indigo_do_nothing_setup(struct gpio_peripheral *periph)
{
	TRACE_ENTRY();

	periph->reset = indigo_gpio_do_nothing;
	periph->power_on = indigo_gpio_do_nothing;
	periph->power_off = indigo_gpio_do_nothing;
	periph->check_and_power_on = indigo_gpio_do_nothing;
	periph->status = indigo_gpio_do_nothing;

	TRACE_EXIT_RES(0);
	return 0;
}
EXPORT_SYMBOL(indigo_do_nothing_setup);

static int indigo_check_and_power_on(struct gpio_peripheral *periph)
{
	int result = 0;

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

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
int gsm_sim508_power_on(struct gpio_peripheral *periph)
{
	int status = 0;
	int result = 0;

	struct indigo_gpio_sequence_step steps[] = {
		{"0", "turn on POWER pin if available",
		 periph, INDIGO_FUNCTION_POWER, 1, false, 0, 0},

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

	sBUG_ON(periph == NULL);
	sBUG_ON(periph->status == NULL);

	/* initially, status pin is 0 -- modem is turned off */
	if (periph->status(periph)) {
		printk(KERN_ERR "tried to power on device with status pin 1");
		result = -ENODEV;
		goto out;
	}

	indigo_gpio_perform_sequence(&steps[0], ARRAY_SIZE(steps));

	status = periph->status(periph);

	PRINT(KERN_ERR, "status pin is %d", status);

	result = !status;
out:
	TRACE_EXIT_RES(result);
	return result; /* 0 is OK code, 1 -- error */
}

/* p.3.4.2.1. figure 4 */
int gsm_sim508_power_off(struct gpio_peripheral *periph)
{
	int status;
	int result = 0;

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

	sBUG_ON(periph == NULL);
	sBUG_ON(periph->status == NULL);

	/* initially, status pin is 1 -- modem is turned on */
	if (!periph->status(periph)) {
		printk(KERN_ERR "tried to power off device with status pin 0");
		result = -ENODEV;
		goto out;
	}

	indigo_gpio_perform_sequence(&steps[0], ARRAY_SIZE(steps));

	status = periph->status(periph);

	PRINT(KERN_ERR, "status pin is %d", status);

	result = status;
out:
	TRACE_EXIT_RES(result);
	return result; /* 0 is OK code, 1 -- is error */
}

int gsm_sim508_setup(struct gpio_peripheral *periph)
{
	int result = 0;

	TRACE_ENTRY();

	periph->reset = indigo_generic_reset;
	periph->power_on = gsm_sim508_power_on;
	periph->power_off = gsm_sim508_power_off;
	periph->status = gsm_generic_status;
	periph->check_and_power_on = indigo_check_and_power_on;

	result = gsm_generic_simcom_setup(periph, keep_turned_on_handler_irq);

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
int gsm_sim900D_power_on(struct gpio_peripheral *periph)
{
	int status = 0;
	int result = 0;

	struct indigo_gpio_sequence_step steps[] = {

		{"0", "turn on POWER pin if available",
		 periph, INDIGO_FUNCTION_POWER, 1, false, 0, 0},

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

	sBUG_ON(periph == NULL);
	sBUG_ON(periph->status == NULL);

	/* initially, status pin is 0 -- modem is turned off */
	if (periph->status(periph)) {
		printk(KERN_ERR "tried to power on device with status pin 1");
		result = -ENODEV;
		goto out;
	}

	indigo_gpio_perform_sequence(&steps[0], ARRAY_SIZE(steps));

	status = periph->status(periph);
	PRINT(KERN_ERR, "status pin is %d", status);

	result = !status;

out:
	TRACE_EXIT_RES(result);
	return result; /* 0 is OK code, 1 -- error */
}

/* figure 10 */
int gsm_sim900D_power_off(struct gpio_peripheral *periph)
{
	int status;
	int result;

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

	sBUG_ON(periph == NULL);
	sBUG_ON(periph->status == NULL);

	/* initially, status pin is 1 -- modem is turned on */
	if (!periph->status(periph)) {
		printk(KERN_ERR "tried to power off device with status pin 0");
		result = -ENODEV;
		goto out;
	}

	indigo_gpio_perform_sequence(&steps[0], ARRAY_SIZE(steps));

	status = periph->status(periph);

	PRINT(KERN_ERR, "status pin is %d", status);

	result = status;
out:
	TRACE_EXIT_RES(result);
	return result; /* 0 is OK code, 1 -- is error */
}

int gsm_sim900D_setup(struct gpio_peripheral *periph)
{
	int result = 0;

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	periph->status = gsm_generic_status;
	periph->power_on = gsm_sim900D_power_on;
	periph->power_off = gsm_sim900D_power_off;
	periph->reset = indigo_generic_reset;
	periph->check_and_power_on = indigo_check_and_power_on;

	result = gsm_generic_simcom_setup(periph, keep_turned_on_handler_irq);

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

static inline void gsm_update_status(struct gpio_peripheral *periph)
{
	TRACE_ENTRY();

	sBUG_ON(periph == NULL);
	sBUG_ON(periph->status == NULL);

	if (periph->status(periph))
		periph->current_state = GPIO_PERIPH_STATE_GSM_ON;
	else
		periph->current_state = GPIO_PERIPH_STATE_GSM_OFF;

	TRACE_EXIT();
	return;
}

/* figure 9, pg 25 */
int gsm_sim900_power_on(struct gpio_peripheral *periph)
{
	int status = 0;
	int result;

	struct indigo_gpio_sequence_step steps[] = {

		{"0", "turn on POWER pin if available",
		 periph, INDIGO_FUNCTION_POWER, 1, false, 0, 0},

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

	sBUG_ON(periph == NULL);
	sBUG_ON(periph->status == NULL);

	/* initially, status pin is 0 -- modem is turned off */
	if (periph->status(periph)) {
		printk(KERN_ERR "tried to power on device with status pin 1");
		result = -ENODEV;
		goto out;
	}

	indigo_gpio_perform_sequence(&steps[0], ARRAY_SIZE(steps));

	status = periph->status(periph);
	PRINT(KERN_ERR, "device status is %d", status);

	gsm_update_status(periph);

	result = !status;
out:
	TRACE_EXIT_RES(result);
	return result; /* 0 is OK code, 1 -- error */
}

int gsm_sim900_power_off(struct gpio_peripheral *periph)
{
	int status;
	int result;

	struct indigo_gpio_sequence_step steps[] = {

		{"1", "pwrkey -> 0 for 5s < t < 1s",
		 periph, INDIGO_FUNCTION_PWRKEY, 0, true, 2000, 0},

		{"2", "set PWRKEY -> 1",
		 periph, INDIGO_FUNCTION_PWRKEY, 1, true, 50, 0},

		/* monitor status pin for value 0 */
		{"3", "wait for > 1.7 seconds",
		 periph, INDIGO_FUNCTION_STATUS, 0, true, 0, 10000},

		{"4", "turn off gsm enable pin",
		 periph, INDIGO_FUNCTION_POWER, 0, true, 1, 0},

		{"5", "finally, status pin is 0 when all is ok",
		 NULL, INDIGO_FUNCTION_NO_FUNCTION, 0, true, 0, 0}
	};

	TRACE_ENTRY();

	/* same as sim900 */

	sBUG_ON(periph == NULL);

	/* initially, status pin is 1 -- modem is turned on */
	if (!periph->status(periph)) {
		printk(KERN_ERR "tried to power off device with status pin 0");
		result = -ENODEV;
		goto out;
	}

	indigo_gpio_perform_sequence(&steps[0], ARRAY_SIZE(steps));

	status = periph->status(periph);
	PRINT(KERN_ERR, "device status is %d", status);

	gsm_update_status(periph);

	result = status;
out:
	TRACE_EXIT_RES(result);
	return result; /* 0 is OK code, 1 -- is error */

}

/**
 * индекс состояния по названию для данного устройства
 * -ENOENT если не нашли
 */
static int state_lookup_by_name(struct gpio_peripheral *periph, const char *name)
{
	int i = 0;
	int found_index = -ENOENT;

	sBUG_ON(periph == NULL);

	if (periph->state_table == NULL) {
		printk(KERN_ERR "searching in non-existent state table for device %s\n", periph->name);
		goto out;
	}

	while (periph->state_table[i].name[0] != '\0' && i < 20 /* safeguard */) {
		if (!strcmp(periph->state_table[i].name, name)) {
			found_index = i;
			break;
		}
		i++;
	}

out:
	return found_index;
}

/*
 * -EINVAL для запрещённых состояний,
 * -EAGAIN для текущих состояний,
 * 0 если получилось,
 * 1 -- не получилось
 *
 * функция синхронная
 */
int gsm_sim900_state_transition(struct gpio_peripheral *periph, int state)
{
	int result = 0;

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	if (periph->current_state == state) {
		printk(KERN_INFO "trying to enter the same state for periph %s\n", periph->name);
		result = -EAGAIN;
		goto out;
	}

	switch (periph->current_state) {
	case GPIO_PERIPH_STATE_GSM_OFF:

		switch (state) {
		case GPIO_PERIPH_STATE_GSM_ON:
			periph->power_on(periph);
			break;
		case GPIO_PERIPH_STATE_GSM_KEEP_ON:
			indigo_set_keep_on_handler(periph, keep_turned_on_handler_irq);
			periph->power_on(periph);
			break;
		case GPIO_PERIPH_STATE_SIM900_FIRMWARE_PREPARE:
			indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_PWRKEY, 0, true);
			msleep(10);
			break;
		case GPIO_PERIPH_STATE_SIM900_FIRMWARE_LOAD:
			result = -EINVAL;
			break;
		};
		break;
	case GPIO_PERIPH_STATE_GSM_ON:
		switch (state) {
		case GPIO_PERIPH_STATE_GSM_OFF:
			periph->power_off(periph);
			break;
		case GPIO_PERIPH_STATE_GSM_KEEP_ON:
			indigo_set_keep_on_handler(periph, keep_turned_on_handler_irq);
			break;
		case GPIO_PERIPH_STATE_SIM900_FIRMWARE_PREPARE:
			periph->power_off(periph);
			indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_PWRKEY, 0, true);
			break;
		case GPIO_PERIPH_STATE_SIM900_FIRMWARE_LOAD:
			result = -EINVAL;
			break;
		}
		break;
	case GPIO_PERIPH_STATE_GSM_KEEP_ON:
		switch (state) {
		case GPIO_PERIPH_STATE_GSM_OFF:
			indigo_set_keep_on_handler(periph, NULL);
			periph->power_off(periph);
			break;
		case GPIO_PERIPH_STATE_GSM_ON:
			indigo_set_keep_on_handler(periph, NULL);
			break;
		case GPIO_PERIPH_STATE_SIM900_FIRMWARE_PREPARE:
			indigo_set_keep_on_handler(periph, NULL);
			periph->power_off(periph);
			indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_PWRKEY, 0, true);
			msleep(10);
			break;
		case GPIO_PERIPH_STATE_SIM900_FIRMWARE_LOAD:
			result = -EINVAL;
			break;
		}
		break;
	case GPIO_PERIPH_STATE_SIM900_FIRMWARE_PREPARE:
		switch (state) {
		case GPIO_PERIPH_STATE_GSM_OFF:
			indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_PWRKEY, 1, true);
			msleep(10);
			break;
		case GPIO_PERIPH_STATE_GSM_ON:
			periph->power_on(periph);
			break;
		case GPIO_PERIPH_STATE_GSM_KEEP_ON:
			periph->power_on(periph);
			indigo_set_keep_on_handler(periph, keep_turned_on_handler_irq);
			break;
		case GPIO_PERIPH_STATE_SIM900_FIRMWARE_LOAD:
			indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_POWER, 1, true);
			break;
		}
		break;
	case GPIO_PERIPH_STATE_SIM900_FIRMWARE_LOAD:
		switch (state) {
		case GPIO_PERIPH_STATE_GSM_OFF:
			indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_POWER, 0, true);
			indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_PWRKEY, 1, true);
			msleep(10);
			break;
		case GPIO_PERIPH_STATE_GSM_ON:
			indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_POWER, 0, true);
			msleep(100); /* ускоренное выключение */
			periph->power_on(periph); /* pwrkey is toggled there */
			break;
		case GPIO_PERIPH_STATE_GSM_KEEP_ON:
			indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_POWER, 0, true);
			msleep(100); /* ускоренное выключение wi*/
			periph->power_on(periph);
			break;
		case GPIO_PERIPH_STATE_SIM900_FIRMWARE_PREPARE:
			indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_POWER, 0, true);
			msleep(100);
			break;
		}
		break;

	default:
		printk(KERN_ERR "non-possible case for state %d\n", periph->current_state);
		panic("non possible case %d for device %s\n", state, periph->name);
	}

	if (!result)
		periph->current_state = state;

out:
	TRACE_EXIT_RES(result);
	return result;

}


int gsm_sim900_setup(struct gpio_peripheral *periph)
{

	int result = 0;

	static struct indigo_gpioperiph_state_desc_t states[] = {
		{"off", GPIO_PERIPH_STATE_GSM_OFF},
		{"on", GPIO_PERIPH_STATE_GSM_ON},
		{"on-keep", GPIO_PERIPH_STATE_GSM_KEEP_ON},
		{"firmware-prepare", GPIO_PERIPH_STATE_SIM900_FIRMWARE_PREPARE},
		{"firmware-load", GPIO_PERIPH_STATE_SIM900_FIRMWARE_LOAD},
		{0, 255}
	};


	TRACE_ENTRY();

	periph->reset = indigo_generic_reset;
	periph->status = gsm_generic_status;
	periph->power_on = gsm_sim900_power_on;
	periph->power_off = gsm_sim900_power_off;
	periph->check_and_power_on = indigo_check_and_power_on;
	periph->state_transition = gsm_sim900_state_transition;

	result = gsm_generic_simcom_setup(periph, NULL);
	if (result) {
		PRINT(KERN_ERR, "error in generic_simcom_setup");
		goto out;
	}

	periph->state_table = kmalloc(sizeof(states), GFP_KERNEL);
	memcpy(periph->state_table, states, sizeof(states));

	indigo_peripheral_create_command_arg(periph,
					INDIGO_COMMAND_STATE_TRANSITION,
					GPIO_PERIPH_STATE_GSM_ON);

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
static int gps_sim508_status(struct gpio_peripheral *periph)
{
	int power_pin;
	int result = 0;

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	power_pin = indigo_gpioperiph_get_pin_by_function(periph, INDIGO_FUNCTION_POWER);

	result = indigo_pin_active_value(&periph->pins[power_pin],
					gpio_get_value(periph->pins[power_pin].pin_no));

	TRACE_EXIT_RES(result);
	return result;
}

/* figure 28 */
int gps_sim508_power_on(struct gpio_peripheral *periph)
{
	int status;
	int result;

	struct indigo_gpio_sequence_step steps[] = {
		{"1", "set power to on and wait 220 ms",
		 periph, INDIGO_FUNCTION_POWER, 1, true, 220, 0},

	};

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);
	sBUG_ON(periph->status == NULL);

	if (periph->status(periph)) {
		printk(KERN_ERR "GPS already seems to work\n");
		result = -ENODEV;
		goto out;
	}

	indigo_gpio_perform_sequence(&steps[0], ARRAY_SIZE(steps));

	status = periph->status(periph);
	PRINT(KERN_ERR, "device status is %d", status);

	result = !status;
out:
	TRACE_EXIT_RES(result);
	return result;
}

/* no precise way to nicely turn this off */
int gps_sim508_power_off(struct gpio_peripheral *periph)
{
	struct indigo_gpio_sequence_step steps[] = {
		{"1", "set power to off and wait some time (500 ms)",
		 periph, INDIGO_FUNCTION_POWER, 0, true, 500, 0},

	};

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);
	sBUG_ON(periph->status == NULL);

	if (!periph->status(periph)) {
		printk(KERN_ERR "GPS already seems to be turned off\n");
		sBUG();
		return -ENODEV;
	}

	indigo_gpio_perform_sequence(&steps[0], ARRAY_SIZE(steps));

	TRACE_EXIT();
	return 0;
}

int gps_sim508_setup(struct gpio_peripheral *periph)
{
	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	indigo_configure_pin(periph, INDIGO_FUNCTION_POWER, /* mandatory */ true);

	indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_POWER, 1, true);

	periph->power_on = gps_sim508_power_on;
	periph->power_off = gps_sim508_power_off;
	periph->status = gps_sim508_status;
	periph->reset = indigo_generic_reset;
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
static int gps_eb500_power_on(struct gpio_peripheral *periph)
{
	int result = 0;

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_POWER, 1, true);
	msleep(200);

	TRACE_EXIT_RES(result);
	return result;
}

static int gps_eb500_power_off(struct gpio_peripheral *periph)
{
	int result = 0;

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_POWER, 0, true);
	msleep(500);

	TRACE_EXIT_RES(result);
	return result;
}

static int gps_eb500_status(struct gpio_peripheral *periph)
{
	int result = 0;
	int power_pin;
	int power_pin_value;

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	power_pin = indigo_gpioperiph_get_pin_by_function(periph,
							INDIGO_FUNCTION_POWER);

	power_pin_value = gpio_get_value(periph->pins[power_pin].pin_no);

	result = (power_pin_value ==
		indigo_pin_active_value(&periph->pins[power_pin], power_pin_value));

	TRACE_EXIT_RES(result);
	return result;
}

int gps_eb500_setup(struct gpio_peripheral *periph)
{
	int result = 0;

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	indigo_configure_pin(periph, INDIGO_FUNCTION_POWER, /* mandatory */ true);

	indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_POWER, 1, true);

	/* FIXME */
	periph->power_on = gps_eb500_power_on;
	periph->power_off = gps_eb500_power_off;
	periph->status = gps_eb500_status;
	periph->reset = indigo_generic_reset;
	periph->check_and_power_on = indigo_check_and_power_on;


	TRACE_EXIT_RES(result);
	return result;
}
EXPORT_SYMBOL(gps_eb500_setup);

/*
 * NV80C-CSM GPS/GNSS Module (Hardware V2.1)
 *
 */

static int gps_nv08c_csm_power_on(struct gpio_peripheral *periph)
{
	int result = 0;

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_POWER, 1, true);
	msleep(200);

	TRACE_EXIT_RES(result);
	return result;
}

int gps_nv08c_csm_power_off(struct gpio_peripheral *periph)
{
	int result = 0;

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	indigo_gpioperiph_set_output(periph, INDIGO_FUNCTION_POWER, 0, true);
	msleep(500);

	TRACE_EXIT_RES(result);
	return result;
}

/*
  2.4.2. Сброс
  Входной сигнал #RESET (#RESET, вывод No25) в NV08C-CSM может быть использован внешней системой
  * для принудительного сброса цифровой части модуля. Для сброса цифровой части модуля внешняя
  * система должна обеспечить на входе #RESET нулевой импульс со следующими характеристиками:
  уровень сигнала не выше 0.3хVCCIO
  длительность импульса не менее 1 мкс.
  При этом встроенный в модуль супервизор будет удерживать цифровую часть модуля в
  состоянии #RESET не менее 140 мс после установки уровня сигнала #RESET из «0» в «1»
*/
int gps_nv08c_csm_reset(struct gpio_peripheral *periph)
{
	struct indigo_gpio_sequence_step steps[] = {

		{"1", "initially, reset is on",
		 periph, INDIGO_FUNCTION_RESET, 1, true, 500, 0},

		{"2", "reset to 0 for 1 ms",
		 periph, INDIGO_FUNCTION_RESET, 0, true, 1, 0},

		{"3", "reset to 1 for 140 ms",
		 periph, INDIGO_FUNCTION_RESET, 1, true, 140, 0},

		{"4", "finally, we have no way to check if everything is ok",
		 NULL, INDIGO_FUNCTION_NO_FUNCTION, 0, true, 0, 0}
	};

	/* FIXME тут лучше, наверно, использовать STATUS_GPS */

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	indigo_gpio_perform_sequence(&steps[0], ARRAY_SIZE(steps));

	TRACE_EXIT_RES(0);
	return 0; /* 0 is OK code, 1 -- is error */
}

static int gps_nv08c_csm_status(struct gpio_peripheral *periph)
{
	int result = 0;
	int power_pin;
	int power_pin_value;

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	power_pin = indigo_gpioperiph_get_pin_by_function(periph,
							INDIGO_FUNCTION_POWER);

	power_pin_value = gpio_get_value(periph->pins[power_pin].pin_no);

	result = indigo_pin_active_value(&periph->pins[power_pin], power_pin_value);

	TRACE_EXIT_RES(result);
	return result;
}

int gps_nv08c_csm_setup(struct gpio_peripheral *periph)
{
	int result = 0;

	TRACE_ENTRY();

	sBUG_ON(periph == NULL);

	periph->status = gps_nv08c_csm_status;
	periph->reset = gps_nv08c_csm_reset;
	periph->power_on = gps_nv08c_csm_power_on;
	periph->power_off = gps_nv08c_csm_power_off;
	periph->check_and_power_on = indigo_check_and_power_on;

	indigo_configure_pin(periph, INDIGO_FUNCTION_RESET, /* mandatory */ true);
	indigo_configure_pin(periph, INDIGO_FUNCTION_POWER, /* mandatory */ true);

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

	if (attribute->store)
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

	TRACE_ENTRY();

	/* execute command */

	gp_cmd = container_of(command, struct gpio_peripheral_command, work);
	peripheral = gp_cmd->peripheral;
	peripheral_obj = container_of(peripheral, struct gpio_peripheral_obj, peripheral);

	/* FIXME how to check NULL here??? < sizeof(struct *)? :-) */

	switch (gp_cmd->cmd) {
	case INDIGO_COMMAND_NO_COMMAND:
		printk(KERN_INFO "NO_COMMAND is issued\n");
		break;
	case INDIGO_COMMAND_POWER_ON:
		sBUG_ON(peripheral->power_on == NULL);
		peripheral->power_on(peripheral);
		break;
	case INDIGO_COMMAND_POWER_OFF:
		sBUG_ON(peripheral->power_off == NULL);
		peripheral->power_off(peripheral);
		break;
	case INDIGO_COMMAND_RESET:
		sBUG_ON(peripheral->reset == NULL);
		peripheral->reset(peripheral);
		break;
	case INDIGO_COMMAND_CHECK_AND_POWER_ON:
		sBUG_ON(peripheral->check_and_power_on == NULL);
		peripheral->check_and_power_on(peripheral);
		break;
	case INDIGO_COMMAND_STATE_TRANSITION:
		sBUG_ON(peripheral->state_transition == NULL);
		peripheral->state_transition(peripheral, gp_cmd->argument);
		break;
	default:
		printk(KERN_ERR "unknown command supplied\n");
	}

	/* сигнализируем страждущим */
	complete(&gp_cmd->complete);

	TRACE_EXIT();
}

/* not safe to free commands inside work struct handler, let's postpone command kfree */
static void indigo_peripheral_free_completed_commands(struct gpio_peripheral_obj *peripheral_obj)
{
	struct gpio_peripheral_command *gp_cmd, *tmp;
	unsigned long flags = 0;

	TRACE_ENTRY();

	// FIXME check locking !!!!!!!!!!!!!!!!!!!!!!!!!

	spin_lock_irqsave(&peripheral_obj->command_list_lock, flags);

	list_for_each_entry_safe(gp_cmd, tmp, &peripheral_obj->command_list, command_sequence) {
		if (completion_done(&gp_cmd->complete)) {
			list_del(&gp_cmd->command_sequence);
			/* FIXME SLUB */
			kmem_cache_free(indigo_cmd_mem_cache, gp_cmd);
		}
	}

	spin_unlock_irqrestore(&peripheral_obj->command_list_lock, flags);

	TRACE_EXIT();
}

struct completion *indigo_peripheral_create_command(struct gpio_peripheral *peripheral,
						enum indigo_gpioperiph_command_t command)
{
	return indigo_peripheral_create_command_arg(peripheral, command, -1);
}

/* создать, поместить в очередь
 *
 * CONTEXT: atomic or process
 */
struct completion *indigo_peripheral_create_command_arg(struct gpio_peripheral *peripheral,
							enum indigo_gpioperiph_command_t command, int argument)
{
	struct gpio_peripheral_command *gp_cmd;
	struct gpio_peripheral_obj *peripheral_obj;

	TRACE_ENTRY();

	printk(KERN_ERR "creating command %d\n", command);

	sBUG_ON(peripheral == NULL);
	peripheral_obj = container_of(peripheral, struct gpio_peripheral_obj, peripheral);

	gp_cmd = kmem_cache_zalloc(indigo_cmd_mem_cache, GFP_KERNEL);
	if (!gp_cmd) {
		printk(KERN_ERR "no memory for gp_cmd\n");
		goto out;
	}

	gp_cmd->cmd = command;
	gp_cmd->peripheral = peripheral;
	gp_cmd->argument = argument;

	INIT_WORK(&gp_cmd->work, indigo_peripheral_process_command);
	INIT_LIST_HEAD(&gp_cmd->command_sequence);
	list_add_tail(&gp_cmd->command_sequence, &peripheral_obj->command_list);
	init_completion(&gp_cmd->complete);

	queue_work(peripheral_obj->wq, &gp_cmd->work);

out:
	TRACE_EXIT();
	return &gp_cmd->complete;
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
	ssize_t len = 0;

	TRACE_ENTRY();

	sBUG_ON(peripheral_obj == NULL);

	periph = &peripheral_obj->peripheral;
	(void) attr;

	if (periph->state_table == NULL && periph->status != NULL)
		len = sprintf(buf, "%s\n", periph->status(periph) == 1 ? "on" : "off");
	else if (periph->state_table != NULL){
		sBUG_ON(periph->current_state < 0);
		len = sprintf(buf, "%s\n", periph->state_table[periph->current_state].name);
	} else
		printk(KERN_ERR "status is not defined\n");

	TRACE_EXIT();
	return len;
}

/* ищем записанное слово в таблице состояний, получаем номер состояния
 * и ждём перехода в состояние */
static ssize_t status_store(struct gpio_peripheral_obj
			*peripheral_obj,
			struct gpio_peripheral_attribute *attr,
			const char *buf, size_t count)
{
	int state_number = -1;
	struct completion *complete;
	char state_name_buf[200];

	(void) attr;

	TRACE_ENTRY();

	sBUG_ON(peripheral_obj == NULL);

	if (peripheral_obj->peripheral.state_table == NULL)
		goto out;

	sscanf(buf, "%s\n", state_name_buf);
	printk(KERN_ERR "got request for state %s\n", state_name_buf);

	state_number = state_lookup_by_name(&peripheral_obj->peripheral, state_name_buf);

	if (state_number < 0) {
		count = -EINVAL;
		goto out;
	}

	complete = indigo_peripheral_create_command_arg(&peripheral_obj->peripheral,
							INDIGO_COMMAND_STATE_TRANSITION,
							state_number);
	wait_for_completion_interruptible(complete);
	indigo_peripheral_free_completed_commands(peripheral_obj);

out:
	TRACE_EXIT();
	return count;
}


#if 0
static ssize_t dummy_store(struct gpio_peripheral_obj *periph_obj, struct gpio_peripheral_attribute *attr,
			const char *buf, size_t count)
{
	(void) periph_obj;
	(void) attr;
	(void) buf;
	return count; /* do nothing at all */
}
#endif

static ssize_t dummy_show(struct gpio_peripheral_obj *periph_obj, struct gpio_peripheral_attribute *attr,
			char *buf)
{
	(void) periph_obj;
	(void) attr;
	(void) buf;
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

	(void) peripheral_obj;
	pin = container_of(attr, struct indigo_periph_pin, sysfs_attr);
	len = sprintf(buf, "%d\n", gpio_get_value(pin->pin_no));

	TRACE_EXIT();
	return len;
}

static ssize_t gpio_store(struct gpio_peripheral_obj *periph_obj,
			struct gpio_peripheral_attribute *attr,
			const char *buf, size_t count)
{
	struct indigo_periph_pin *pin;
	int value;
	int len;

	(void) periph_obj;
	(void) attr;
	(void) buf;

	TRACE_ENTRY();

	pin = container_of(attr, struct indigo_periph_pin, sysfs_attr);

	/* GPIOF_DIR_OUT is a 0 in first bit, 1 means DIR_IN */
	if ((pin->flags & GPIOF_DIR_IN) != 0) {
		printk(KERN_ERR "not allowing to set value of input pin\n");
		count = -EINVAL;
		goto out;
	}

	len = sscanf(buf, "%d\n", &value);
	if (len != 1) {
		count = -EINVAL;
		goto out;
	}

	gpio_set_value(pin->pin_no, value);

out:
	TRACE_EXIT();
	return count; /* do nothing at all */
}


static ssize_t power_on_store(struct gpio_peripheral_obj *peripheral_obj, struct gpio_peripheral_attribute *attr,
			const char *buf, size_t count)
{
	struct completion *complete;
	TRACE_ENTRY();

	sBUG_ON(peripheral_obj == NULL);

	(void) buf;
	(void) attr;

	complete = indigo_peripheral_create_command(&peripheral_obj->peripheral, INDIGO_COMMAND_POWER_ON);
	wait_for_completion_interruptible(complete);
	indigo_peripheral_free_completed_commands(peripheral_obj);


	TRACE_EXIT();
	return count;
}

static ssize_t check_and_power_on_store(struct gpio_peripheral_obj *peripheral_obj, struct gpio_peripheral_attribute *attr,
					const char *buf, size_t count)
{
	struct completion *complete;
	TRACE_ENTRY();

	sBUG_ON(peripheral_obj == NULL);

	(void) buf;
	(void) attr;

	complete = indigo_peripheral_create_command(&peripheral_obj->peripheral, INDIGO_COMMAND_CHECK_AND_POWER_ON);
	wait_for_completion_interruptible(complete);
	indigo_peripheral_free_completed_commands(peripheral_obj);

	TRACE_EXIT();
	return count;
}

static ssize_t power_off_store(struct gpio_peripheral_obj *peripheral_obj, struct gpio_peripheral_attribute *attr,
			const char *buf, size_t count)
{
	struct completion *complete;
	TRACE_ENTRY();

	sBUG_ON(peripheral_obj == NULL);

	(void) buf;
	(void) attr;

	complete = indigo_peripheral_create_command(&peripheral_obj->peripheral, INDIGO_COMMAND_POWER_OFF);
	wait_for_completion_interruptible(complete);
	indigo_peripheral_free_completed_commands(peripheral_obj);

	TRACE_EXIT();
	return count;

}

static ssize_t reset_store(struct gpio_peripheral_obj *peripheral_obj, struct gpio_peripheral_attribute *attr,
			const char *buf, size_t count)
{
	struct completion *complete;
	TRACE_ENTRY();

	sBUG_ON(peripheral_obj == NULL);

	(void) buf;
	(void) attr;

	complete = indigo_peripheral_create_command(&peripheral_obj->peripheral, INDIGO_COMMAND_RESET);
	wait_for_completion_interruptible(complete);
	indigo_peripheral_free_completed_commands(peripheral_obj);

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
	__ATTR(status, 0666, status_show, status_store),
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
	struct gpio_peripheral_obj *peripheral_obj = NULL;
	struct sysfs_dirent *value_sd = NULL;
	int retval;
	int i;

	if (peripheral->name == NULL)
		goto out;

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

	INIT_WORK(&peripheral_obj->check_status_work, indigo_check_status);

	for (i = 0; i < INDIGO_MAX_GPIOPERIPH_PIN_COUNT; i++) {
		if (peripheral->pins[i].description == NULL)
			break;

		/* read-only attribute */
		peripheral->pins[i].sysfs_attr.attr.name = peripheral->pins[i].schematics_name;
		peripheral->pins[i].sysfs_attr.attr.mode = 0666;
		peripheral->pins[i].sysfs_attr.show = gpio_show;
		peripheral->pins[i].sysfs_attr.store = gpio_store;
		/* add file to sysfs. not too sure about rolling back */
		retval = sysfs_create_file(&peripheral_obj->kobj, &peripheral->pins[i].sysfs_attr.attr);
		if (retval) {
			printk(KERN_ERR "error creating sysfs file\n");
			continue;
		};

		if (peripheral->pins[i].flags & GPIOF_POLLABLE) {
			/* first, get struct sysfs_dirent for current attribute */
			value_sd = sysfs_get_dirent(peripheral_obj->kobj.sd, NULL, peripheral->pins[i].schematics_name);
			peripheral->pins[i].value_sd = value_sd;

			if (value_sd == NULL)
				printk(KERN_ERR "couldn't get sysfs dirent for pin %s\n",
					peripheral->pins[i].schematics_name);

			INIT_WORK(&peripheral->pins[i].work, indigo_pin_notify_sysfs);

			/* second, register the interrupt handler */
			if (request_irq(gpio_to_irq(peripheral->pins[i].pin_no),
						indigo_pin_notify_change_handler,
						IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
						peripheral->pins[i].schematics_name,
						(void *) &peripheral->pins[i].work)) {

				printk(KERN_ERR "couldn't set up change handler for pin %s\n",
					peripheral->pins[i].schematics_name);
			}
		}
	}

	/* in order to release all of 'em */
	INIT_LIST_HEAD(&peripheral_obj->kobject_item);
	list_add_tail(&peripheral_obj->kobject_item, &kobjects);

	/* ------ specific for each device ---------- */
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

/* здесь вообще нужно выставить имя, а в борде сказать, что мы хотим драйвер с этим именем и вот его platform_data */

/* давайте пока забьём на модули -- нужно делать много деинициализаии, если его выгружать :-( */
#if 0
static struct platform_device indigo_gpioperiph_device = {
	.name           = "indigo_gpioperiph",
	.id             = -1,
	.dev            = {
		.platform_data = &indigo_gpioperiph_platform_data,
	}
};
#endif

/**
 * Entry point of driver
 */
int indigo_gpio_peripheral_init(struct gpio_peripheral peripherals[3], int nr_devices)
{
	int result;
	int i;
	struct gpio_peripheral_obj *periph_obj;

	/*
	 * Create a kset with the name of "kset_example",
	 * located under /sys/kernel/
	 */
	indigo_kset = kset_create_and_add("indigo", NULL, kernel_kobj);
	if (!indigo_kset) {
		result = -EIO;
		goto out;
	}

	memcpy(&indigo_gpioperiph_platform_data, peripherals, sizeof(peripherals[0]) * 3);

	indigo_cmd_mem_cache = kmem_cache_create("indigo_periph_cmd",
						sizeof(struct gpio_peripheral_command),
						0,
						SLAB_POISON | SLAB_RED_ZONE,
						NULL);
	if (indigo_cmd_mem_cache == NULL) {
		printk(KERN_ERR "could'nt init mem cache\n");
		result = -ENOMEM;
		goto out;
	}


	for (i = 0; i < nr_devices; i++) {
		printk(KERN_ERR "adding device %s\n", peripherals[i].description);
		periph_obj = create_gpio_peripheral_obj(&peripherals[i]);
		if (!periph_obj) {
			printk(KERN_ERR "fatal error during object creation\n");
			result = -EINVAL;
			goto out;
		}

		printk(KERN_ERR "indigo gpioperiph: %s peripheral %s added\n",
			periph_obj->peripheral.name, periph_obj->peripheral.description);
	}

	//	platform_device_register(&indigo_gpioperiph_device);


out:
	return result;
}

void indigo_gpio_peripheral_exit(void)
{
	struct gpio_peripheral_obj *obj, *tmp;

	kmem_cache_destroy(indigo_cmd_mem_cache);

	/* we need to correctly destroy all objects here, not sure about attributes */
	list_for_each_entry_safe(obj, tmp, &kobjects, kobject_item) {
		destroy_gpio_peripheral_obj(obj);
		list_del(&obj->kobject_item);
	}
	kset_unregister(indigo_kset);
}

EXPORT_SYMBOL(indigo_gpio_peripheral_init);
EXPORT_SYMBOL(indigo_gpio_peripheral_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yury Luneff <yury@indigosystem.ru>");
MODULE_DESCRIPTION("Generic GPIO peripheral driver with some predefined peripherals");
