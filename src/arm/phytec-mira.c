#include <stdlib.h>
#include <stdbool.h>
#include <syslog.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <glob.h>

#include <arm/phytec-mira.h>

#define CONFIGURATION_FILE	"/etc/mraa.map"

#define ARRAY_SIZE(_a)	(sizeof(_a) / sizeof(_a)[0])

#define DEBUG	0

#define dprint(fmt, ...)				\
	do {						\
		if ((DEBUG))				\
			printf(fmt, ## __VA_ARGS__);	\
	} while (0)

struct phytec_mira_board {
	mraa_board_t	mraa;
	unsigned long	spi_devices;
};

static void freec(void const *p)
{
	free((void *)p);
}

static bool reserve_pins(struct phytec_mira_board *board,
			 size_t start, size_t cnt)
{
	mraa_pininfo_t	*pins;
	size_t		num = start + cnt;
	size_t		i;

	if (UINT_MAX - cnt < start || UINT_MAX / sizeof *pins <= num) {
		syslog(LOG_ERR, "invalid pin range %u+%u",
		       start, cnt);
		return false;
	}

	if (num > board->mraa.phy_pin_count) {
		pins = realloc(board->mraa.pins,
			       num * sizeof board->mraa.pins[0]);
		if (!pins) {
			syslog(LOG_ERR, "failed to allocate %zu pins: %s",
			       num, strerror(errno));
			return false;
		}

		/* zero the newly allocated memory */
		memset(&pins[board->mraa.phy_pin_count], 0,
		       (uintptr_t)(&pins[num]) -
		       (uintptr_t)(&pins[board->mraa.phy_pin_count]));

		board->mraa.phy_pin_count = num;
		board->mraa.pins = pins;
	}

	for (i = start; i < num; ++i) {
		if (board->mraa.pins[i].capabilites.valid) {
			syslog(LOG_ERR,
			       "pin %zu already marked as valid while requesting %zu+%zu range",
			       i, start, num);
			return false;
		}
	}

	return true;
}

static char const *strip(char *str, size_t len)
{
	while (len > 0 && isspace(*str)) {
		++str;
		--len;
	}

	while (len > 0 && isspace(str[len-1]))
		--len;

	str[len] = '\0';
	return str;
}

static char const *find_device(char const *pattern)
{
	glob_t		globbuf;
	int		rc;
	char const	*sysfs_name;
	char const	*base;

	rc = glob(pattern, GLOB_ERR, NULL, &globbuf);
	if (rc != 0)
		return NULL;

	if (globbuf.gl_pathc != 1) {
		syslog(LOG_ERR, "bad count %zu of matches for '%s'",
		       globbuf.gl_pathc, pattern);
		sysfs_name = NULL;
	} else {
		sysfs_name = globbuf.gl_pathv[0];
	}

	if (!sysfs_name) {
		base = NULL;
	} else {
		/* TODO: read the sysfs entries ('dev') instead of taking the
		 * basename of the directory */
		base = strrchr(sysfs_name, '/');
		if (!base)
			syslog(LOG_ERR, "strange systfs name '%s'", sysfs_name);
		else
			/* 'base' pointed to the '/'; move to the name */
			++base;

		if (base && !*base) {
			syslog(LOG_ERR, "invalid sysfs '%s'; trailing slash?",
			       sysfs_name);
			base = NULL;
		}
	}

	if (base)
		base = strdup(base);

	globfree(&globbuf);

	return base;
}

/* parse lines of the format:

   GPIO <pin-number> <bank> <io-idx> [<name>]

   Code assumes that a bank contains 32 gpios and '<io-idx>' is within [0..31]
   range
*/
static bool parse_gpio(char const *s, struct phytec_mira_board *board)
{
	unsigned int	idx;
	unsigned int	bank;
	unsigned int	pin;
	char const	*name;
	int		rc;
	mraa_pininfo_t	*pin_ptr;

	dprint("%s(%s)\n",  __func__, s);

	rc = sscanf(s, "%u %u %u %ms", &idx, &bank, &pin, &name);
	if (rc < 3)
		/* TODO: implement sysfs paths? */
		return false;

	if (rc < 4)
		name = NULL;

	if (!reserve_pins(board, idx, 1))
		return false;

	pin_ptr = &board->mraa.pins[idx];
	*pin_ptr = (mraa_pininfo_t){
		.capabilites = {
			.valid	= true,
			.gpio	= true,
		},
		.gpio = {
			.pinmap	= (bank-1) * 32 + pin,
		},
	};

	if (name)
		snprintf(pin_ptr->name, sizeof pin_ptr->name, "%s", name);

	dprint("added gpio #%u @%zu\n", board->mraa.pins[idx].gpio.pinmap, idx);

	return true;
}

/* parses lines of the format

   SPI <spi-idx> <bus> <ss>
   SPI <spi-idx> <sysfs-glob>

   The expanded <sysfs-glob> must end with 'spidev<bus>.<cs>'
 */
static bool parse_spi(char const *s, struct phytec_mira_board *board)
{
	unsigned int	idx;
	unsigned int	bus;
	unsigned int	ss;

	int		rc;
	mraa_spi_bus_t	*spi;

	dprint("%s(%s)\n",  __func__, s);

	rc = sscanf(s, "%u %u %u", &idx, &bus, &ss);

	/* use a 'while' to handle errors within the loop more effectively */
	while (rc != 3) {
		char const	*sysfs = NULL;
		char const	*device;

		rc = sscanf(s, "%u %ms", &idx, &sysfs);
		if (rc < 0 || !sysfs)
			break;

		device = find_device(sysfs);
		freec(sysfs);

		if (!device) {
			rc = -1;
			break;
		}

		rc = sscanf(device, "spidev%u.%u", &bus, &ss);
		freec(device);

		if (rc != 2)
			rc = -1;

		break;
	}

	if (rc < 0) {
		syslog(LOG_ERR, "failed to handle '%s'", s);
		return false;
	}

	if (idx >= ARRAY_SIZE(board->mraa.spi_bus)) {
		syslog(LOG_ERR, "index %zu out of SPI range", idx);
		return false;
	}

	if (board->spi_devices & (1u << idx)) {
		syslog(LOG_ERR, "spi device %zu already defined", idx);
		return false;
	}

	spi = &board->mraa.spi_bus[idx];
	spi->bus_id  = bus;
	spi->slave_s = ss;

	dprint("added SPI %u.%u @%zu\n", bus, ss, idx);

	return true;
}

/* parses lines of the format

   PWM <pin-number> <sysfs-glob> <pwm-idx> [<name>]

   The expanded <sysfs-glob> must end with 'pwmchip<id>'
 */
static bool parse_pwm(char const *s, struct phytec_mira_board *board)
{
	unsigned int	pin;
	char const	*chip_path;
	unsigned int	chip_id;
	unsigned int	pwm_idx;
	char const	*name;

	int		rc;
	mraa_pininfo_t	*pin_ptr;

	dprint("%s(%s)\n",  __func__, s);

	rc = sscanf(s, "%u %ms %u %ms", &pin, &chip_path, &pwm_idx, &name);
	if (rc < 2)
		chip_path = NULL;
	if (rc < 3)
		rc = -1;

	/* use a 'while' to handle errors within the loop more effectively */
	while (rc >= 3) {
		char const	*device;

		if (rc < 4)
			name = NULL;

		rc = -1;

		device = find_device(chip_path);
		if (!device)
			break;

		rc = sscanf(device, "pwmchip%u", &chip_id);
		freec(device);

		if (rc != 1)
			break;

		rc = 0;
		break;
	}

	freec(chip_path);

	if (rc < 0) {
		syslog(LOG_ERR, "failed to handle '%s'", s);
		return false;
	}

	if (!reserve_pins(board, pin, 1)) {
		freec(name);
		return false;
	}

	pin_ptr = &board->mraa.pins[pin];
	*pin_ptr = (mraa_pininfo_t){
		.capabilites = {
			.valid	= true,
			.pwm	= true,
		},
		.pwm = {
			.pinmap		= pwm_idx,
			.parent_id	= chip_id,
		},
	};

	if (name)
		snprintf(pin_ptr->name, sizeof pin_ptr->name, "%s", name);

	freec(name);

	dprint("added PWM #%u.%u @%zu (%s)\n",
	       pin_ptr->pwm.parent_id, pin_ptr->pwm.pinmap, pin,
	       pin_ptr->name);


	return true;
}

/* parses lines of the format

   UART <uart-number> <sysfs-glob>

 */
static bool parse_uart(char const *s, struct phytec_mira_board *board)
{
	unsigned int	idx;
	char const	*sysfs_path;
	int		rc;
	char const	*device;
	mraa_uart_dev_t	*uart_ptr;

	dprint("%s(%s)\n",  __func__, s);

	rc = sscanf(s, "%u %ms", &idx, &sysfs_path);
	if (rc != 2)
		return false;

	if (idx >= ARRAY_SIZE(board->mraa.uart_dev)) {
		syslog(LOG_ERR, "uart index %zu out of range", idx);
		uart_ptr = NULL;
	} else {
		uart_ptr = &board->mraa.uart_dev[idx];
	}

	if (uart_ptr && uart_ptr->device_path) {
		syslog(LOG_ERR, "uart #%zu already setup (%s)", idx,
		       uart_ptr->device_path);
		uart_ptr = NULL;
	}

	device = find_device(sysfs_path);
	freec(sysfs_path);

	if (!device)
		return false;

	if (uart_ptr) {
		char	*tmp;

		tmp = malloc(strlen(device) + sizeof "/dev/");
		if (tmp) {
			strcpy(tmp, "/dev/");
			strcat(tmp, device);
			uart_ptr->index = idx;
		}

		uart_ptr->device_path = tmp;
	}


	freec(device);

	if (uart_ptr == NULL || uart_ptr->device_path == NULL)
		return false;


	dprint("added UART '%s' at %zu\n", uart_ptr->device_path, idx);

	return true;
}

static bool parse_configuration(char const *fname,
				struct phytec_mira_board *board)
{
	FILE	*fh;
	char	*buf = NULL;
	size_t	len = 0;
	bool	ret;

	fh = fopen(fname, "r");
	if (!fh)
		/* nothing to parse... */
		return 0;

	ret = true;
	while (!feof(fh)) {
		ssize_t		n;
		char const	*line;

		n = getline(&buf, &len, fh);
		if (n < 0)
			break;

		line = strip(buf, n);

		if (line[0] == '#' || line[0] == '\0')
			/* comment or empty lines; ignore */
			ret = true;
		else if (strncmp(line, "GPIO", 4) == 0)
			ret = parse_gpio(line+4, board);
		else if (strncmp(line, "SPI", 3) == 0)
			ret = parse_spi(line+4, board);
		else if (strncmp(line, "PWM", 3) == 0)
			ret = parse_pwm(line+4, board);
		else if (strncmp(line, "UART", 4) == 0)
			ret = parse_uart(line+4, board);
		else
			ret = false;

		if (!ret) {
			syslog(LOG_ERR, "failed to parse '%s'", line);
			break;
		}
	}

	fclose(fh);

	return ret;
}

static size_t count_gpios(struct phytec_mira_board const *board)
{
	size_t		i;
	size_t		res = 0;

	for (i = 0; i < board->mraa.phy_pin_count; ++i) {
		if (board->mraa.pins[i].capabilites.valid &&
		    board->mraa.pins[i].capabilites.gpio)
			++res;
	}

	return res;
}

mraa_board_t *mraa_phytec_mira(void)
{
	struct phytec_mira_board	*board = NULL;
	size_t				i;

	board = malloc(sizeof *board);
	if (!board) {
		syslog(LOG_ERR, "failed to allocate board object: %s",
		       strerror(errno));
		goto out;
	}

	*board = (struct phytec_mira_board) {
		.mraa	= {
			.platform_name	= "Phytec MIRA",
			.no_bus_mux	= true,
			/* this assumes, that nobody adds a PCIe or USB device
			 * which provides an extra I2C bus... */
			.i2c_bus_count	= 3,
		},
	};

	for (i = 0; i < board->mraa.i2c_bus_count; ++i) {
		board->mraa.i2c_bus[i] = (mraa_i2c_bus_t) {
			.bus_id = i,
		};
	}

	if (!parse_configuration(CONFIGURATION_FILE, board)) {
		free(board->mraa.pins);
		free(board);

		board = NULL;
	}

	if (board)
		board->mraa.gpio_count = count_gpios(board);

out:
	return board ? &board->mraa : NULL;
}

int main(void)
{
	mraa_phytec_mira();
}
