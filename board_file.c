struct gpio_periph indigo_all_peripherals [][] = {
	{ /* revision 0 */
		{
			.kind = INDIGO_PERIPH_KIND_GSM,
			.name = "gsm",
			.description = "Sim508 GSM",
			.setup = gsm_sim508_setup,
			.pins = {
				{
					.kind = INDIGO_FUNCTION_STATUS,
					.schematics_name = "STATUS_GSM",
					.description = "sim508 status pin",
					.pin_no = AT91_PIN_PA22,
					.flags = GPIOF_DIR_INPUT | GPIOF_PULLUP | GPIOF_DEGLITCH | GPIOF_ACTIVE_HIGH
				},
				{
					.kind = INDIGO_FUNCTION_PWRKEY,
					.schematics_name = "PWRkey",
					.description = "sim508 power key pin",
					.pin_no = AT91_PIN_PA23,
					.flags = GPIOF_DIR_OUTPUT | GPIOF_INIT_LOW
				}
			}
		}
	},
	{ /* revision DEVICE_STARTERKIT -- 1 */
	},
	{ /* revision DEVICE_1_0 -- 2 */
	},
	{ /* revision DEVICE_1_1 -- 3 */
		{
			.kind = INDIGO_PERIPH_KIND_GPS,
			.name = "gps",
			.description = "NV08C-CSM",
			.setup = gps_nv08c_csm_setup,
			.pins = {
				{
					.kind = INDIGO_FUNCTION_NO_FUNCTION,
					.schematics_name = "NET_ANT",
					.description = "1 соответствует подключению ко входу активной антенны, 0 – отсутствию нагрузки",
					.pin_no = AT91_PIN_PC11,
					.flags = GPIOF_DIR_INPUT | GPIOF_PULLUP
				}
			}
		},
		{
			.kind = INDIGO_PERIPH_KIND_POWER,
			.name = "power",
			.description = "LM-something",
			.setup = setup_power,
			.pins = {
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

void board_init()
{
	/* need to show how our device init should work */
	int i;

	struct gpio_peripheral *periph;

	indigo_gpio_peripheral_init();

	for (i = 0; i < ARRAY_SIZE(indigo_all_peripherals[system_rev]); i++) {
		periph = create_gpio_peripheral_obj(indigo_all_peripherals[system_rev][i]);
		printk(KERN_ERR "indigo gpioperiph: %s peripheral %s added\n",
		       periph->name, periph->description);
	}
}
