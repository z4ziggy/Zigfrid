TARGET     ?= zigfrid
DEVICE     := attiny85
PROGRAMMER := -c buspirate -P /dev/ttyUSB0 -x cpufreq=125
FUSES      := -U lfuse:w:0xC0:m
FUSES_RESET:= -U lfuse:w:0x62:m 

#AVRDUDE = avrdude -p attiny85 -c usbtiny -U flash:w:file.hex:i 
AVRDUDE = avrdude $(PROGRAMMER) -p $(DEVICE)
COMPILE = avr-gcc -mmcu=$(DEVICE) -Wall -Os -std=gnu99  -Wno-volatile-register-var
#LIBS    = -nostdlib
# -lgcc -lc

OBJECTS = $(TARGET).o

all:	$(TARGET).hex

.c.o:
	$(COMPILE) -c $< -o $@

.S.o:
	$(COMPILE) -x assembler-with-cpp -c $< -o $@

.c.s:
	$(COMPILE) -S $< -o $@

flash:	all
	$(AVRDUDE) -U flash:w:$(TARGET).hex:i

fuse:
	$(AVRDUDE) $(FUSES)

reset:
	$(AVRDUDE) $(FUSES_RESET)

install: flash fuse

load: all
	bootloadHID $(TARGET).hex

clean:
	rm -f $(TARGET).hex $(TARGET).elf $(OBJECTS)

%.elf: $(OBJECTS)
	$(COMPILE) -o $(TARGET).elf $(OBJECTS) $(LIBS)
	avr-size --mcu=$(DEVICE) --format=avr $(TARGET).elf

%.hex: $(TARGET).elf
	rm -f $(TARGET).hex
	avr-objcopy -j .text -j .data -O ihex $(TARGET).elf $(TARGET).hex

disasm:	$(TARGET).elf
	avr-objdump -D $(TARGET).elf

cpp:
	$(COMPILE) -E $(TARGET).c

