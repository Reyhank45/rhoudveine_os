.PHONY: all clean menuconfig build-x86_64

all: build-x86_64

build-x86_64:
	$(MAKE) -C kernel build-x86_64

clean:
	$(MAKE) -C kernel clean

menuconfig:
	$(MAKE) -C kernel menuconfig
