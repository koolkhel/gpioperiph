#ifndef _INDIGO_GPIOPERIPH_H
#define _INDIGO_GPIOPERIPH_H

#include <linux/gpio.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/list.h>

#define INDIGO_MAX_GPIOPERIPH_PIN_COUNT 32
#define INDIGO_NO_PIN 255

/*
 make these flag values available regardless of GPIO kconfig options
#define GPIOF_DIR_OUT   (0 << 0)
#define GPIOF_DIR_IN    (1 << 0)

#define GPIOF_INIT_LOW  (0 << 1)
#define GPIOF_INIT_HIGH (1 << 1)
*/

#define GPIOF_PULLUP (1 << 2)
#define GPIOF_NOPULLUP (0 << 2)

#define GPIOF_DEGLITCH (1 << 3)
#define GPIOF_NODEGLITCH (0 << 3)

#define GPIOF_ACTIVE_LOW (1 << 4)
#define GPIOF_ACTIVE_HIGH (0 << 4)

enum indigo_pin_flags_t {
	INDIGO_GPIO_INPUT = 0x1,
	INDIGO_GPIO_OUTPUT = 0x2,
	INDIGO_GPIO_INVERTED = 0x4 /* если inverted,
				    * то 0 – активное значение, 1 – неактивное,
				    * иначе – необорот */,
	INDIGO_GPIO_DEGLICH = 0x8
};

enum indigo_pin_function_t {
	INDIGO_FUNCTION_NO_FUNCTION, /* ну просто индикатор */
	INDIGO_FUNCTION_POWER, /* пин управляет ключом, способным обесточить девайс*/
	INDIGO_FUNCTION_PWRKEY, /* пин управляет входом включения устройства на самом устройстве*/
	INDIGO_FUNCTION_RESET, /* судя по всему, не нужен, его эмулирует PWRKEY */
	INDIGO_FUNCTION_STATUS /* для GSM на этой ножке
				* надо обрабатывать прерывания */
};

enum indigo_gpioperiph_kind_t {
	INDIGO_PERIPH_KIND_UNKNOWN,
	INDIGO_PERIPH_KIND_GSM,
	INDIGO_PERIPH_KIND_GPS,
	INDIGO_PERIPH_KIND_POWER
};

enum indigo_gpioperiph_command_t {
	INDIGO_COMMAND_NO_COMMAND,
	INDIGO_COMMAND_POWER_ON,
	INDIGO_COMMAND_POWER_OFF,
	INDIGO_COMMAND_RESET,
	INDIGO_COMMAND_CHECK_AND_POWER_ON /* проверить статус и если 0 -- включить */
};

struct gpio_peripheral_attribute;

struct indigo_periph_pin {
	const char *description; /* “power_key”, “status”, “NET_ANT” */

	const char *schematics_name; /* симлинк с названием из схемы, тогда будет */
	int pin_no;
	indigo_pin_function_t function; /* <- по этому буду искать какой пин
					 * дёрнуть при включении девайса,
					 * как ресурс примерно */
	int flags; /* INVERT, GPIO_INPUT, GPIO_OUTPUT */

	struct gpio_peripheral_attribute sysfs_attr;
};

/*
 * all functions except setup are supposed to be called
 * from user thread thus we are allowed to make synchronized calls.
 */

struct gpio_peripheral {
	indigo_gpioperiph_kind_t kind; /* 1 == GSM, 2 == GPS, 3 == ? */
	const char *name; /* “gsm”, “gps”, маленькими буквами */
	const char *description; /* “Sim900 GSM”, “NVC08-CSM”, etc */
	void *(*setup)(struct gpio_peripheral *);
	void *(*power_on)(struct gpio_peripheral *); /*как включить устройство */
	void *(*power_off)(struct gpio_peripheral *); /* как выключить устройство */
	void *(*reset)(struct gpio_peripheral *); /* перевключить устройство */
	void *(*status)(struct gpio_peripheral *); /* 1 -- включено, 0 -- выключено */

	/* необходимые для основных операций над устройством */
	struct indigo_periph_pin pins[INDIGO_MAX_GPIOPERIPH_PIN_COUNT];

	/* куда-то ещё нужны списки аттрибутов, мб поиск по имени ещё */
};

struct gpio_peripheral_obj {
	struct kobject kobj; /* dynamic allocation required */
	struct gpio_peripheral peripheral;

	struct list_head kobject_item;
	struct list_head command_list; // INIT_LIST_HEAD()
	/* всё, что надо инициализировать в куче -- в _obj, создавать в конструкторе */
	/* очередь команд, выделять через alloc_workqueue,
	 * max_active = 1 */
	struct workqueue_struct wq;
	struct completion command_list_empty;
	// init_completion(); при создании
	// INIT_COMPLETION(); при повторном использовании, а оно у нас будет
	struct spinlock_t command_list_lock; // spin_lock_init

};
#define to_gpio_peripheral_obj(x) container_of(x, struct gpio_peripheral_obj, kobj)

struct gpio_peripheral_command {
	indigo_gpioperiph_command_t cmd;

	struct gpio_peripheral *peripheral;

	struct work_struct work;

	struct list_head command_sequence;
};

struct indigo_gpio_sequence_step {
	const char *step_no;
	const char *step_desc;
	const gpio_peripheral *periph; /* NULL for trace step */
	indigo_pin_function_t function;
	int value;
	int mandatory;
	int sleep_ms;
	int timeout_ms;
};

/* a custom attribute that works just for a struct foo_obj. */
struct gpio_peripheral_attribute {
        struct attribute attr;
        ssize_t (*show)(struct gpio_peripheral *peripheral,
			struct gpio_peripheral_attribute *attr,
			char *buf);
        ssize_t (*store)(struct gpio_peripheral *peripheral,
			 struct gpio_peripheral_attribute *attr,
			 const char *buf, size_t count);
};
#define to_gpio_peripheral_attr(x) container_of(x, struct gpio_peripheral_attribute, attr)

/*
 * Функции power_on и т.п. должны быть синхронные,
 * может быть нужен какой-то минимальный общий фреймворк для этого.
 *
 *
 В итоге описываться будет примерно так:
 struct gpio_peripheral device_1_1_peripherals [] = {
 {
	.kind = INDIGO_PERIPH_KIND_GSM,
	.name = “gsm”, <- это появится в /sys/indigo/…
	.description = “SimCOM 300D”,
	.power_on = sim300_power_on, <- echo 1 > /sys/indigo/gsm/power_on
	.power_off = sim300_power_off <- echo 1 > /sys/indigo/gsm/power_off
	.reset = sim300_reset <- echo 1 > /sys/indigo/gsm/reset
 },
 {
 ...
 }
 *
 struct gpio_peripheral device_2_1_peripherals [] = {
 * ...
 }
 *
 Выигрыш ещё в общих функциях ресета, которые будут на разных девайсах
}
*/

/* найти для текущего модема ножку power */
int indigo_gpioperiph_get_pin_by_function(struct gpio_peripheral *periph,
	indigo_pin_function_t function);

#endif /* _INDIGO_GPIOPERIPH_H */
