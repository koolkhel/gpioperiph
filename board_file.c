#include "./indigo-gpioperiph.h"

struct gpio_peripheral indigo_all_peripherals [7][3] = {
	{ /* revision 0 */
	},
	{ /* revision DEVICE_STARTERKIT -- 1 */
		{
			.kind = INDIGO_PERIPH_KIND_GSM,
			.name = "gsm",
			.description = "Sim508 GSM",
			.setup = gsm_sim508_setup,
			.pins = {
				{
					.function = INDIGO_FUNCTION_STATUS,
					.schematics_name = "STATUS_GSM",
					.description = "sim508 status pin",
					.pin_no = AT91_PIN_PA22,
					.flags = GPIOF_DIR_IN | GPIOF_PULLUP | GPIOF_DEGLITCH | GPIOF_ACTIVE_HIGH
				},
				{
					.function = INDIGO_FUNCTION_PWRKEY,
					.schematics_name = "PWRkey",
					.description = "sim508 power key pin",
					.pin_no = AT91_PIN_PA23,
					.flags = GPIOF_DIR_OUT | GPIOF_INIT_LOW
				}
			}
		},
		{
			.kind = INDIGO_PERIPH_KIND_GPS,
			.name = "gps",
			.description = "sim508 GPS",
			.setup = gps_sim508_setup,
			.pins = {
				{
					.function = INDIGO_FUNCTION_POWER,
					.schematics_name = "EN_GPS",
					.description = "enable GPS module",
					.pin_no = AT91_PIN_PC5,
					.flags = GPIOF_DIR_OUT | GPIOF_INIT_LOW | GPIOF_ACTIVE_HIGH
				}
			}
		},
	},
	{ /* revision DEVICE_1_0 -- 2 */
		{
			.kind = INDIGO_PERIPH_KIND_GPS,
			.name = "gps",
			.description = "EB-500 GPS",
			.setup = gps_eb500_setup,
			.pins = {
				{
					.function = INDIGO_FUNCTION_POWER,
					.schematics_name = "EN_GPS",
					.description = "enable GNSS module",
					.pin_no = AT91_PIN_PC5,
					.flags = GPIOF_DIR_OUT | GPIOF_INIT_LOW | GPIOF_ACTIVE_HIGH
				},
				{
					.function = INDIGO_FUNCTION_NO_FUNCTION,
					.schematics_name = "V28D",
					.description = "хз что это",
					.pin_no = AT91_PIN_PC6,
					.flags = GPIOF_DIR_IN | GPIOF_PULLUP
				}
			}
		},
		{
			.kind = INDIGO_PERIPH_KIND_GSM,
			.name = "gsm",
			.description = "Sim900D GSM",
			.setup = gsm_sim900_setup,
			.pins = {
				{
					.function = INDIGO_FUNCTION_STATUS,
					.schematics_name = "STATUS_GSM",
					.description = "Sim900D status pin",
					.pin_no = AT91_PIN_PA18,
					.flags = GPIOF_DIR_IN | GPIOF_PULLUP | GPIOF_DEGLITCH | GPIOF_ACTIVE_HIGH
				},
				{
					.function = INDIGO_FUNCTION_PWRKEY,
					.schematics_name = "GSM_ON",
					.description = "Sim900D power key pin",
					.pin_no = AT91_PIN_PA19,
					.flags = GPIOF_DIR_OUT | GPIOF_INIT_LOW
				},
				{
					.function = INDIGO_FUNCTION_POWER,
					.schematics_name = "EN_GSM_ON",
					.description = "Sim900D power pin",
					.pin_no = AT91_PIN_PA17,
					.flags = GPIOF_DIR_OUT | GPIOF_INIT_LOW
				}
			}
		},
		{
			.kind = INDIGO_PERIPH_KIND_POWER,
			.name = "power",
			.description = "LM-something",
			.setup = indigo_configure_general_pins,
			.pins = {
				{
					.function = INDIGO_FUNCTION_NO_FUNCTION,
					.schematics_name = "STAT1",
					.description = "Статус режиме работы:precharge in progress(S1-ON, S2-ON), fast charge in progress(S1-ON, S2-OFF), Charge done(S1-OFF, S2-ON),Charge suspend(S1-OFF, S2-OFF).",
					.pin_no = AT91_PIN_PA24,
					.flags = GPIOF_DIR_IN | GPIOF_PULLUP
				},
				{
					.function = INDIGO_FUNCTION_NO_FUNCTION,
					.schematics_name = "STAT2",
					.description = "Статус режиме работы:precharge in progress(S1-ON, S2-ON), fast charge in progress(S1-ON, S2-OFF), Charge done(S1-OFF, S2-ON),Charge suspend(S1-OFF, S2-OFF).",
					.pin_no = AT91_PIN_PA25,
					.flags = GPIOF_DIR_IN | GPIOF_PULLUP
				},
				{
					.function = INDIGO_FUNCTION_NO_FUNCTION,
					.schematics_name = "Acpg",
					.description = "состояние внешнего источника питания. 0 - хорошо, 1 - плохо",
					.pin_no = AT91_PIN_PA26,
					.flags = GPIOF_DIR_IN | GPIOF_PULLUP
				},
				{
					.function = INDIGO_FUNCTION_NO_FUNCTION,
					.schematics_name = "on_off_sensor",
					.description = "хз что это",
					.pin_no = AT91_PIN_PA27,
					.flags = GPIOF_DIR_IN | GPIOF_PULLUP
				},
			}

		}
	},
	{ /* revision DEVICE_1_1 -- 3 */
		{
			.kind = INDIGO_PERIPH_KIND_GPS,
			.name = "gps",
			.description = "NV08C-CSM GNSS",
			.setup = gps_nv08c_csm_setup,
			.pins = {
				{
					.function = INDIGO_FUNCTION_NO_FUNCTION,
					.schematics_name = "NET_ANT",
					.description = "1 соответствует подключению ко входу активной антенны, 0 – отсутствию нагрузки",
					.pin_no = AT91_PIN_PC11,
					.flags = GPIOF_DIR_IN | GPIOF_PULLUP
				},
				{
					.function = INDIGO_FUNCTION_RESET,
					.schematics_name = "RST_GPS",
					.description = "reset pin, active is low",
					.pin_no = AT91_PIN_PC9,
					.flags = GPIOF_DIR_IN | GPIOF_PULLUP | GPIOF_ACTIVE_LOW
				},
				{
					.function = INDIGO_FUNCTION_POWER,
					.schematics_name = "EN_GPS",
					.description = "enable GNSS module",
					.pin_no = AT91_PIN_PC5,
					.flags = GPIOF_DIR_OUT | GPIOF_INIT_LOW | GPIOF_ACTIVE_HIGH
				}
			}
		},
		{
			.kind = INDIGO_PERIPH_KIND_POWER,
			.name = "power",
			.description = "LM-something",
			.setup = indigo_configure_general_pins,
			.pins = {
				{
					.function = INDIGO_FUNCTION_NO_FUNCTION,
					.schematics_name = "STAT1",
					.description = "Статус режиме работы:precharge in progress(S1-ON, S2-ON), fast charge in progress(S1-ON, S2-OFF), Charge done(S1-OFF, S2-ON),Charge suspend(S1-OFF, S2-OFF).",
					.pin_no = AT91_PIN_PA28,
					.flags = GPIOF_DIR_IN | GPIOF_PULLUP
				},
				{
					.function = INDIGO_FUNCTION_NO_FUNCTION,
					.schematics_name = "STAT2",
					.description = "Статус режиме работы:precharge in progress(S1-ON, S2-ON), fast charge in progress(S1-ON, S2-OFF), Charge done(S1-OFF, S2-ON),Charge suspend(S1-OFF, S2-OFF).",
					.pin_no = AT91_PIN_PA29,
					.flags = GPIOF_DIR_IN | GPIOF_PULLUP
				},
				{
					.function = INDIGO_FUNCTION_NO_FUNCTION,
					.schematics_name = "Acpg",
					.description = "состояние внешнего источника питания. 0 - хорошо, 1 - плохо",
					.pin_no = AT91_PIN_PA29,
					.flags = GPIOF_DIR_IN | GPIOF_PULLUP
				},
				{
					.function = INDIGO_FUNCTION_NO_FUNCTION,
					.schematics_name = "on_off_sensor",
					.description = "хз, что это",
					.pin_no = AT91_PIN_PA29,
					.flags = GPIOF_DIR_IN | GPIOF_PULLUP
				},
			}
		},
		{
			.kind = INDIGO_PERIPH_KIND_GSM,
			.name = "gsm",
			.description = "Sim900 GSM",
			.setup = gsm_sim900_setup,
			.pins = {
				{
					.function = INDIGO_FUNCTION_STATUS,
					.schematics_name = "STATUS_GSM",
					.description = "Sim900 status pin",
					.pin_no = AT91_PIN_PC7,
					.flags = GPIOF_DIR_IN | GPIOF_PULLUP | GPIOF_DEGLITCH | GPIOF_ACTIVE_HIGH
				},
				{
					.function = INDIGO_FUNCTION_PWRKEY,
					.schematics_name = "PWRkey",
					.description = "Sim900 power key pin",
					.pin_no = AT91_PIN_PC4,
					.flags = GPIOF_DIR_OUT | GPIOF_INIT_LOW
				},
				{
					.function = INDIGO_FUNCTION_POWER,
					.schematics_name = "EN_GSM",
					.description = "Sim900 power key pin",
					.pin_no = AT91_PIN_PC10,
					.flags = GPIOF_DIR_OUT | GPIOF_INIT_LOW
				}

			}
		}
	},
	{ /* revision DEVICE_2_0 -- 4 */
	},
	{ /* revision DEVICE_2_1 -- 5 */
	},
	{ /* revision DEVICE_STARTERKIT_9G45 -- 6 */
	}
};

void board_init(void)
{
	/* need to show how our device init should work */
	unsigned int i;

	struct gpio_peripheral_obj *periph_obj;

	indigo_gpio_peripheral_init();

	for (i = 0; i < ARRAY_SIZE(indigo_all_peripherals[system_rev]); i++) {
		periph_obj = create_gpio_peripheral_obj(&indigo_all_peripherals[system_rev][i]);
		printk(KERN_ERR "indigo gpioperiph: %s peripheral %s added\n",
		       periph_obj->peripheral.name, periph_obj->peripheral.description);
	}
}
