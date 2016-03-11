Phytec MIRA
===========

The Phytec MIRA board with i.MX6 processor is identified by a devicetree
`compatible` entry matching "fsl,imx6" (TODO: use the more specialized
"phytec,pcm058" instead of?).

Pins and devices are not setup statically but will be read at runtime
from `/etc/mraa.map`.  This file has the following syntax:

```
### Comment
# <text>

### GPIO
GPIO <pin-number> <bank> <io-idx> [<name>]

### SPI; <sysfs-glob> must end with 'spidev<bus>.<cs>'
SPI  <spi-idx> <bus> <ss>
SPI  <spi-idx> <sysfs-glob>

### PWM; <sysfs-glob> must end with 'pwmchip<id>'
PWM  <pin-number> <sysfs-glob> <pwm-idx> [<name>]

### UART; base of <sysfs-glob> will be searched below /dev
UART <uart-number> <sysfs-glob>
```

An example file might be

```
GPIO	0	2 3	nRESET
PWM	1	/sys/devices/soc0/soc/2000000.aips-bus/2080000.pwm/pwm/pwmchip* 0 TEST
SPI	1	/sys/devices/soc0/soc/2000000.aips-bus/2000000.spba-bus/200c000.ecspi/spi_master/spi2/spi2.0/spidev/spidev2.0
SPI	0	/sys/devices/soc0/soc/2000000.aips-bus/2000000.spba-bus/2008000.ecspi/spi_master/spi1/spi1.2/spidev/spidev1.2
UART	1	/sys/devices/soc0/soc/2100000.aips-bus/21e8000.serial/tty/tty*
UART	0	/sys/devices/soc0/soc/2100000.aips-bus/21ec000.serial/tty/tty*
```
