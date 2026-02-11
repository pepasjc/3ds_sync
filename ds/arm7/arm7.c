#include <calico.h>
#include <nds.h>

int main() {
	// Read settings from NVRAM
	envReadNvramSettings();

	// Set up extended keypad server (X/Y/hinge)
	keypadStartExtServer();

	// Configure and enable VBlank interrupt
	lcdSetIrqMask(DISPSTAT_IE_ALL, DISPSTAT_IE_VBLANK);
	irqEnable(IRQ_VBLANK);

	// Set up RTC
	rtcInit();
	rtcSyncTime();

	// Initialize power management
	pmInit();

	// Set up block device peripherals
	blkInit();

	// Set up touch screen driver
	touchInit();
	touchStartServer(80, MAIN_THREAD_PRIO);

	// Set up sound and mic driver
	soundStartServer(MAIN_THREAD_PRIO-0x10);
	micStartServer(MAIN_THREAD_PRIO-0x18);

	// Set up wireless manager
	wlmgrStartServer(MAIN_THREAD_PRIO-8);

	// Keep the ARM7 mostly idle
	while (pmMainLoop()) {
		threadWaitForVBlank();
	}

	return 0;
}
